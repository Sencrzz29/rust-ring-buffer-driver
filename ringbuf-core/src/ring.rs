// SPDX-License-Identifier: GPL-2.0

//! Panic-free circular byte buffer.
//!
//! This file is the *logic* of the driver. It depends only on `core`, so the
//! exact same source is
//!   * compiled into the kernel module (`drivers/rust/rustring.rs` pulls it in
//!     with `#[path = ...] mod ring;`), and
//!   * unit- and property-tested on the host with `cargo test`.
//!
//! # Defensive ("Yes+P") rules, enforced by the lints below
//!
//! * no `buf[i]` / `buf[a..b]` — every access goes through `.get()` /
//!   `.get_mut()` / `split_at_mut_checked()`, which return `Option` instead of
//!   panicking (`clippy::indexing_slicing`);
//! * no unchecked `+ - * / %` — every computation uses `checked_*` and turns
//!   overflow into an error (`clippy::arithmetic_side_effects`);
//! * no `unwrap`, `expect`, `panic!`, `unreachable!`, `todo!` ...;
//! * no `unsafe` at all (`forbid(unsafe_code)`).
//!
//! In user space a panic kills one process. In the kernel it is a `BUG()`
//! that can take down the whole board, so every failure here is a value of
//! [`RingError`] that the driver turns into an errno for the caller.
//!
//! # Representation
//!
//! `head` is the physical index of the oldest byte, `len` the number of
//! stored bytes. Invariants (kept by every method):
//!
//! * `len <= capacity`
//! * `head < capacity`, or `head == 0` when `capacity == 0`
//!
//! Storing `len` (rather than a `tail` index) means "full" and "empty" are
//! never ambiguous, so no slot is wasted.

#![forbid(unsafe_code)]
#![deny(
    clippy::indexing_slicing,
    clippy::arithmetic_side_effects,
    clippy::unwrap_used,
    clippy::expect_used,
    clippy::panic,
    clippy::unreachable,
    clippy::todo,
    clippy::unimplemented,
    clippy::cast_possible_truncation
)]

use core::ops::{Deref, DerefMut};

/// Smallest capacity accepted by [`check_capacity`].
pub const MIN_CAPACITY: usize = 64;
/// Largest capacity accepted by [`check_capacity`] (4 MiB).
pub const MAX_CAPACITY: usize = 4_194_304;

/// Everything that can go wrong. The driver maps each variant to an errno.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RingError {
    /// No space for even one more byte (driver: `-EAGAIN`).
    Full,
    /// Offset/length outside the stored data, or an arithmetic overflow while
    /// computing it (driver: `-EINVAL`).
    OutOfRange,
    /// Capacity outside `[MIN_CAPACITY, MAX_CAPACITY]` (driver: `-EINVAL`).
    BadCapacity,
    /// The operation needs an empty buffer (driver: `-EBUSY`).
    Busy,
}

/// Validates a capacity requested by user space.
///
/// Takes a `u64` because that is what crosses the ioctl boundary; on a 32-bit
/// kernel a huge value must not be silently truncated into a small one.
pub fn check_capacity(requested: u64) -> Result<usize, RingError> {
    let cap = usize::try_from(requested).map_err(|_| RingError::BadCapacity)?;
    if (MIN_CAPACITY..=MAX_CAPACITY).contains(&cap) {
        Ok(cap)
    } else {
        Err(RingError::BadCapacity)
    }
}

/// Copies as many bytes as fit from `src` into the front of `dst`.
/// Returns the number of bytes copied.
fn copy_prefix(dst: &mut [u8], src: &[u8]) -> usize {
    let n = dst.len().min(src.len());
    match (dst.get_mut(..n), src.get(..n)) {
        // Both slices have length exactly `n`, so `copy_from_slice` cannot panic.
        (Some(d), Some(s)) => {
            d.copy_from_slice(s);
            n
        }
        _ => 0,
    }
}

/// A FIFO byte ring over caller-provided storage.
///
/// `S` is anything that dereferences to a byte slice: `KVVec<u8>` in the
/// kernel, `Vec<u8>` in host tests. The ring never allocates by itself, so
/// allocation failure is handled (fallibly) by whoever creates the storage.
pub struct RingBuf<S> {
    storage: S,
    head: usize,
    len: usize,
}

impl<S> RingBuf<S>
where
    S: Deref<Target = [u8]> + DerefMut,
{
    /// Creates an empty ring whose capacity is `storage.len()`.
    pub fn new(storage: S) -> Self {
        RingBuf {
            storage,
            head: 0,
            len: 0,
        }
    }

    /// Total number of bytes the ring can hold.
    pub fn capacity(&self) -> usize {
        self.storage.len()
    }

    /// Number of bytes currently stored.
    pub fn len(&self) -> usize {
        self.len
    }

    /// `true` if nothing is stored.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Number of bytes that can still be written.
    pub fn free(&self) -> usize {
        self.capacity().saturating_sub(self.len)
    }

    /// Discards all stored bytes.
    pub fn clear(&mut self) {
        self.head = 0;
        self.len = 0;
    }

    /// Gives the storage back (e.g. to replace it on resize).
    pub fn into_storage(self) -> S {
        self.storage
    }

    /// Physical index of the byte `offset` positions after the read head.
    fn phys(&self, offset: usize) -> Result<usize, RingError> {
        self.head
            .checked_add(offset)
            .and_then(|i| i.checked_rem(self.capacity()))
            .ok_or(RingError::OutOfRange)
    }

    /// The `count` stored bytes that start `offset` bytes after the read
    /// head, as (up to) two slices because the range may wrap around the end
    /// of the storage. Nothing is consumed.
    ///
    /// This is the function an attacker reaches through the PEEK ioctl with
    /// arbitrary `offset` and `count`; every step is checked.
    pub fn peek_slices(&self, offset: usize, count: usize) -> Result<(&[u8], &[u8]), RingError> {
        let end = offset.checked_add(count).ok_or(RingError::OutOfRange)?;
        if end > self.len {
            return Err(RingError::OutOfRange);
        }
        if count == 0 {
            return Ok((&[], &[]));
        }
        let start = self.phys(offset)?;
        let to_end = self
            .capacity()
            .checked_sub(start)
            .ok_or(RingError::OutOfRange)?;
        let first_len = count.min(to_end);
        let second_len = count.checked_sub(first_len).ok_or(RingError::OutOfRange)?;
        let first_end = start.checked_add(first_len).ok_or(RingError::OutOfRange)?;

        let first = self.storage.get(start..first_end).ok_or(RingError::OutOfRange)?;
        let second = self.storage.get(..second_len).ok_or(RingError::OutOfRange)?;
        Ok((first, second))
    }

    /// All stored bytes, oldest first, as (up to) two slices.
    pub fn readable(&self) -> Result<(&[u8], &[u8]), RingError> {
        self.peek_slices(0, self.len)
    }

    /// Drops the `n` oldest bytes (after they were copied out).
    pub fn consume(&mut self, n: usize) -> Result<(), RingError> {
        if n > self.len {
            return Err(RingError::OutOfRange);
        }
        if n == 0 {
            return Ok(());
        }
        let new_head = self.phys(n)?;
        self.len = self.len.checked_sub(n).ok_or(RingError::OutOfRange)?;
        // When the ring drains, restart at index 0 so the next data is
        // contiguous (one copy instead of two).
        self.head = if self.len == 0 { 0 } else { new_head };
        Ok(())
    }

    /// The free space, in write order, as (up to) two mutable slices.
    /// Fill a prefix of them, then call [`commit`](Self::commit).
    pub fn writable_mut(&mut self) -> Result<(&mut [u8], &mut [u8]), RingError> {
        let free = self.free();
        if free == 0 {
            return Ok((&mut [], &mut []));
        }
        let tail = self.phys(self.len)?;
        let to_end = self
            .capacity()
            .checked_sub(tail)
            .ok_or(RingError::OutOfRange)?;
        let first_len = free.min(to_end);
        let second_len = free.checked_sub(first_len).ok_or(RingError::OutOfRange)?;

        // `split_at_mut` panics if `tail > len`; the explicit check makes that
        // branch unreachable. (`split_at_mut_checked` would say this directly
        // but needs Rust 1.80, and Linux 6.18 supports Rust >= 1.78.)
        if tail > self.storage.len() {
            return Err(RingError::OutOfRange);
        }
        let (lo, hi) = self.storage.split_at_mut(tail);
        let first = hi.get_mut(..first_len).ok_or(RingError::OutOfRange)?;
        let second = lo.get_mut(..second_len).ok_or(RingError::OutOfRange)?;
        Ok((first, second))
    }

    /// Marks `n` bytes, previously written into [`writable_mut`]'s slices, as
    /// stored.
    ///
    /// [`writable_mut`]: Self::writable_mut
    pub fn commit(&mut self, n: usize) -> Result<(), RingError> {
        if n > self.free() {
            return Err(RingError::OutOfRange);
        }
        self.len = self.len.checked_add(n).ok_or(RingError::OutOfRange)?;
        Ok(())
    }

    /// Appends as much of `src` as fits. Returns the number of bytes stored,
    /// or [`RingError::Full`] if not even one byte fits.
    pub fn push(&mut self, src: &[u8]) -> Result<usize, RingError> {
        if src.is_empty() {
            return Ok(0);
        }
        let (a, b) = self.writable_mut()?;
        if a.is_empty() {
            return Err(RingError::Full);
        }
        let n1 = copy_prefix(a, src);
        let n2 = copy_prefix(b, src.get(n1..).unwrap_or(&[]));
        let total = n1.checked_add(n2).ok_or(RingError::OutOfRange)?;
        self.commit(total)?;
        Ok(total)
    }

    /// Moves up to `dst.len()` of the oldest bytes into `dst`. Returns the
    /// number of bytes moved (0 if empty).
    pub fn pop(&mut self, dst: &mut [u8]) -> Result<usize, RingError> {
        let n = self.peek(0, dst)?;
        self.consume(n)?;
        Ok(n)
    }

    /// Copies up to `dst.len()` bytes starting `offset` bytes after the read
    /// head into `dst`, without consuming. Unlike the PEEK ioctl this
    /// clamps the length to what is available after `offset`.
    pub fn peek(&self, offset: usize, dst: &mut [u8]) -> Result<usize, RingError> {
        let avail = self.len.checked_sub(offset).ok_or(RingError::OutOfRange)?;
        let count = dst.len().min(avail);
        let (a, b) = self.peek_slices(offset, count)?;
        let n1 = copy_prefix(dst, a);
        let n2 = copy_prefix(dst.get_mut(n1..).unwrap_or(&mut []), b);
        n1.checked_add(n2).ok_or(RingError::OutOfRange)
    }
}

// SPDX-License-Identifier: GPL-2.0

//! Kernel side of `rustring`.
//!
//! One ring is shared by every process that opens `/dev/rustring` (like a
//! pipe), so a producer and a consumer can talk through it.
//!
//! # Locking
//!
//! * `RING`  -- a `Mutex`. Reads/writes copy to and from user memory while
//!   holding it, and a user copy can page-fault and *sleep*. Sleeping under a
//!   spinlock is a classic kernel protocol bug ("scheduling while atomic")
//!   that the Rust compiler cannot see, so the sleepable lock is chosen by
//!   design (and `CONFIG_DEBUG_ATOMIC_SLEEP` in the debug kernel checks it).
//! * `STATS` -- a `SpinLock`: tiny critical sections that never sleep.
//! * Order: `STATS` may be taken on its own or after `RING` is released;
//!   never `RING` while holding `STATS` (lockdep checks this).
//!
//! In both cases the data is *inside* the lock: the only way to reach it is
//! through the guard returned by `lock()`, and the guard unlocks in its
//! `Drop` -- on every return path, including every `?`.
//!
//! # Unsafe budget
//!
//! The 6.18 `global_lock!` API needs a one-time `unsafe` initialisation.
//! That single `unsafe` block in `RustRingModule::init` is the only one in
//! this driver; everything else is safe Rust over the kernel crate's
//! abstractions.

#![deny(
    clippy::indexing_slicing,
    clippy::arithmetic_side_effects,
    clippy::unwrap_used,
    clippy::expect_used,
    clippy::panic,
    clippy::undocumented_unsafe_blocks
)]

use core::mem::size_of;
use core::pin::Pin;

use kernel::{
    c_str,
    fs::{File, Kiocb},
    ioctl::{_IO, _IOR, _IOW},
    iov::{IovIterDest, IovIterSource},
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    prelude::*,
    str::CStr,
    uaccess::{UserPtr, UserSlice},
};

use crate::ring::{check_capacity, RingBuf, RingError};

const NAME: &str = "rustring";
const DEVICE_NAME: &CStr = c_str!("rustring");

/// Capacity at load time (64 KiB) -- `RB_DEFAULT_CAPACITY` in the uapi header.
const DEFAULT_CAPACITY: usize = 65_536;

// ioctl numbers -- must match include/uapi/ringbuf.h.
const RB_IOC_MAGIC: u32 = 0xB5;
/// `struct rb_stats` is five `__u64`s.
const RB_IOC_GET_STATS: u32 = _IOR::<[u64; 5]>(RB_IOC_MAGIC, 1);
const RB_IOC_CLEAR: u32 = _IO(RB_IOC_MAGIC, 2);
const RB_IOC_RESIZE: u32 = _IOW::<u64>(RB_IOC_MAGIC, 3);
/// `struct rb_peek` is three `__u64`s.
const RB_IOC_PEEK: u32 = _IOW::<[u64; 3]>(RB_IOC_MAGIC, 4);

type Ring = RingBuf<KVVec<u8>>;

/// Everything protected by the ring mutex.
struct RingState {
    /// `None` until module init has allocated the buffer, and again once
    /// module exit has freed it -- so a late caller gets `-ENODEV`, never a
    /// use-after-free.
    ring: Option<Ring>,
}

/// Counters reported by `RB_IOC_GET_STATS`.
#[derive(Clone, Copy)]
struct Stats {
    bytes_written: u64,
    bytes_read: u64,
    rejected: u64,
}

kernel::sync::global_lock! {
    // SAFETY: `RING.init()` runs in `RustRingModule::init` before the misc
    // device is registered, i.e. before anything can call `RING.lock()`.
    unsafe(uninit) static RING: Mutex<RingState> = RingState { ring: None };
}

kernel::sync::global_lock! {
    // SAFETY: as for `RING`.
    unsafe(uninit) static STATS: SpinLock<Stats> = Stats {
        bytes_written: 0,
        bytes_read: 0,
        rejected: 0,
    };
}

impl From<RingError> for Error {
    fn from(e: RingError) -> Self {
        match e {
            RingError::Full => EAGAIN,
            RingError::OutOfRange | RingError::BadCapacity => EINVAL,
            RingError::Busy => EBUSY,
        }
    }
}

fn to_u64(n: usize) -> u64 {
    u64::try_from(n).unwrap_or(u64::MAX)
}

/// Updates the counters after a read/write: bytes on success, `rejected`
/// on error.
fn record(res: &Result<usize>, counter: fn(&mut Stats) -> &mut u64) {
    let mut stats = STATS.lock();
    match res {
        Ok(n) => {
            let c = counter(&mut stats);
            *c = c.saturating_add(to_u64(*n));
        }
        Err(_) => stats.rejected = stats.rejected.saturating_add(1),
    }
}

// ---------------------------------------------------------------------------
// read / write
// ---------------------------------------------------------------------------

fn do_read(iov: &mut IovIterDest<'_>) -> Result<usize> {
    let mut state = RING.lock();
    let ring = state.ring.as_mut().ok_or(ENODEV)?;

    let (first, second) = ring.readable()?;
    let had_data = !first.is_empty();
    // Copy straight from the ring into the user's buffer(s); no bounce copy.
    let mut copied = iov.copy_to_iter(first);
    if copied == first.len() {
        copied = copied
            .checked_add(iov.copy_to_iter(second))
            .ok_or(EINVAL)?;
    }
    if copied == 0 && had_data && !iov.is_empty() {
        // Data was available and the caller asked for some, yet nothing
        // could be copied: the user buffer is bad.
        return Err(EFAULT);
    }
    ring.consume(copied)?;
    Ok(copied)
}

fn do_write(iov: &mut IovIterSource<'_>) -> Result<usize> {
    if iov.is_empty() {
        return Ok(0);
    }
    let mut state = RING.lock();
    let ring = state.ring.as_mut().ok_or(ENODEV)?;

    let (first, second) = ring.writable_mut()?;
    if first.is_empty() {
        return Err(EAGAIN);
    }
    let first_len = first.len();
    let mut copied = iov.copy_from_iter(first);
    if copied == first_len {
        copied = copied
            .checked_add(iov.copy_from_iter(second))
            .ok_or(EINVAL)?;
    }
    if copied == 0 {
        return Err(EFAULT);
    }
    ring.commit(copied)?;
    Ok(copied)
}

// ---------------------------------------------------------------------------
// ioctl
// ---------------------------------------------------------------------------

fn get_stats(arg: usize) -> Result<isize> {
    let (capacity, used) = {
        let state = RING.lock();
        let ring = state.ring.as_ref().ok_or(ENODEV)?;
        (ring.capacity(), ring.len())
    };
    let s = *STATS.lock();
    let values = [
        to_u64(capacity),
        to_u64(used),
        s.bytes_written,
        s.bytes_read,
        s.rejected,
    ];
    // Copy out with no lock held. A bad pointer is -EFAULT, not a crash.
    let mut out = UserSlice::new(UserPtr::from_addr(arg), size_of::<[u64; 5]>()).writer();
    for v in values {
        out.write(&v)?;
    }
    Ok(0)
}

fn clear() -> Result<isize> {
    let mut state = RING.lock();
    state.ring.as_mut().ok_or(ENODEV)?.clear();
    Ok(0)
}

fn resize(arg: usize) -> Result<isize> {
    let requested: u64 = UserSlice::new(UserPtr::from_addr(arg), size_of::<u64>())
        .reader()
        .read()?;
    let capacity = check_capacity(requested)?;
    // Fallible allocation, done before taking the lock (kvmalloc may sleep
    // for a while). Failure is -ENOMEM to the caller.
    let storage = KVVec::from_elem(0u8, capacity, GFP_KERNEL)?;

    let mut state = RING.lock();
    if !state.ring.as_ref().ok_or(ENODEV)?.is_empty() {
        return Err(EBUSY); // `storage` is freed on this return path by Drop
    }
    // The old ring (and its buffer) is dropped -- i.e. kvfree()d -- here.
    state.ring = Some(RingBuf::new(storage));
    Ok(0)
}

fn peek(arg: usize) -> Result<isize> {
    let mut req = UserSlice::new(UserPtr::from_addr(arg), size_of::<[u64; 3]>()).reader();
    let offset: u64 = req.read()?;
    let len: u64 = req.read()?;
    let buf: u64 = req.read()?;

    // User-controlled numbers: convert with checks, never with `as`.
    let offset = usize::try_from(offset).map_err(|_| EINVAL)?;
    let len = usize::try_from(len).map_err(|_| EINVAL)?;
    let buf = usize::try_from(buf).map_err(|_| EFAULT)?;
    let result = isize::try_from(len).map_err(|_| EINVAL)?;

    let state = RING.lock();
    let ring = state.ring.as_ref().ok_or(ENODEV)?;
    // All range/overflow checking happens in ring.rs -> -EINVAL.
    let (first, second) = ring.peek_slices(offset, len)?;
    let mut out = UserSlice::new(UserPtr::from_addr(buf), len).writer();
    out.write_slice(first)?;
    out.write_slice(second)?;
    Ok(result)
}

// ---------------------------------------------------------------------------
// misc device + module
// ---------------------------------------------------------------------------

/// Per-open-file state. Empty: all state is in the shared ring.
pub(crate) struct RustRing;

#[vtable]
impl MiscDevice for RustRing {
    type Ptr = Pin<KBox<Self>>;

    fn open(_file: &File, _misc: &MiscDeviceRegistration<Self>) -> Result<Pin<KBox<Self>>> {
        Ok(KBox::pin(RustRing, GFP_KERNEL)?)
    }

    fn read_iter(_kiocb: Kiocb<'_, Self::Ptr>, iov: &mut IovIterDest<'_>) -> Result<usize> {
        let res = do_read(iov);
        record(&res, |s| &mut s.bytes_read);
        res
    }

    fn write_iter(_kiocb: Kiocb<'_, Self::Ptr>, iov: &mut IovIterSource<'_>) -> Result<usize> {
        let res = do_write(iov);
        record(&res, |s| &mut s.bytes_written);
        res
    }

    fn ioctl(_me: Pin<&RustRing>, _file: &File, cmd: u32, arg: usize) -> Result<isize> {
        let res = match cmd {
            RB_IOC_GET_STATS => get_stats(arg),
            RB_IOC_CLEAR => clear(),
            RB_IOC_RESIZE => resize(arg),
            RB_IOC_PEEK => peek(arg),
            _ => Err(ENOTTY),
        };
        if res.is_err() {
            let mut stats = STATS.lock();
            stats.rejected = stats.rejected.saturating_add(1);
        }
        res
    }
}

/// Module state. Dropping it (on `rmmod`) deregisters `/dev/rustring`.
pub(crate) struct RustRingModule {
    _miscdev: Pin<KBox<MiscDeviceRegistration<RustRing>>>,
}

impl kernel::Module for RustRingModule {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        // SAFETY: `init` runs exactly once, at load time, and the device node
        // that is the only route to RING/STATS is registered further down,
        // so nothing can use the locks before they are initialised.
        unsafe {
            RING.init();
            STATS.init();
        }

        let storage = KVVec::from_elem(0u8, DEFAULT_CAPACITY, GFP_KERNEL)?;
        RING.lock().ring = Some(RingBuf::new(storage));

        let opts = MiscDeviceOptions { name: DEVICE_NAME };
        let miscdev = match KBox::pin_init(MiscDeviceRegistration::register(opts), GFP_KERNEL) {
            Ok(m) => m,
            Err(e) => {
                // Statics are never dropped, so undo the allocation by hand
                // on this error path (the one place RAII can't do it for us).
                RING.lock().ring = None;
                return Err(e);
            }
        };

        pr_info!("loaded, /dev/{} capacity {} bytes\n", NAME, DEFAULT_CAPACITY);
        Ok(Self { _miscdev: miscdev })
    }
}

impl Drop for RustRingModule {
    fn drop(&mut self) {
        // No file can be open here: every open file holds a reference on the
        // module, and rmmod only gets this far when that count is zero.
        // Even so, a late caller would find `None` and get -ENODEV.
        let freed = RING.lock().ring.take().map_or(0, |r| r.capacity());
        pr_info!("unloading, freed {} byte buffer\n", freed);
        // `_miscdev` is dropped after this body: the device is deregistered.
    }
}

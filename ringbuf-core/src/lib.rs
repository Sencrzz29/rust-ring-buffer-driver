// SPDX-License-Identifier: GPL-2.0

//! Host-side wrapper crate around `ring.rs`, so the ring-buffer logic used by
//! the kernel driver can be tested with `cargo test` on a normal machine.
//!
//! `no_std` on purpose: if this crate builds, the code only uses `core` and
//! will also build inside the kernel.

#![no_std]

mod ring;

pub use ring::*;

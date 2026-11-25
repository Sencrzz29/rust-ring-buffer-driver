// SPDX-License-Identifier: GPL-2.0

//! `rustring`: a panic-resilient ring-buffer character device in safe Rust.
//!
//! Crate root. The pieces are:
//! * `ring`: the buffer logic, shared with the host tests
//!   (`ringbuf-core/src/ring.rs`, `no_std`, no `unsafe`);
//! * `driver`: the kernel side -- misc device, locking, user copies, errno
//!   mapping, module lifetime.

use kernel::prelude::*;

// The host crate exposes more of the ring API (e.g. `push`/`pop` for tests)
// than the driver uses.
#[allow(dead_code, unreachable_pub)]
#[path = "../../ringbuf-core/src/ring.rs"]
mod ring;

mod driver;

use driver::RustRingModule;

module! {
    type: RustRingModule,
    name: "rustring",
    authors: ["Gaurav Gupta", "Sujeet Venkata Raman"],
    description: "Panic-resilient ring-buffer character device (safe Rust)",
    license: "GPL",
}

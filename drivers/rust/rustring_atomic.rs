// SPDX-License-Identifier: GPL-2.0

//! `rustring_atomic`: the driver built with `--cfg atomic_bug`, which puts the
//! ring under a spinlock while user copies (which may sleep) happen inside it.
//! A positive control showing that the debug kernel catches a protocol bug
//! that the Rust compiler accepts. Never use it otherwise.

use kernel::prelude::*;

#[allow(dead_code, unreachable_pub)]
#[path = "../../ringbuf-core/src/ring.rs"]
mod ring;

mod driver;

use driver::RustRingModule;

module! {
    type: RustRingModule,
    name: "rustring_atomic",
    authors: ["Gaurav Gupta", "Sujeet Venkata Raman"],
    description: "Ring-buffer device with a sleep-under-spinlock bug (debug-kernel positive control)",
    license: "GPL",
}

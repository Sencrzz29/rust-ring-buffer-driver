// SPDX-License-Identifier: GPL-2.0

//! `rustring_naive`: the same driver built with `--cfg naive`, which swaps in
//! unchecked PEEK and RESIZE implementations (see the bottom of `driver.rs`).
//! Exists only for the crash-resilience experiment -- never use it otherwise.

use kernel::prelude::*;

#[allow(dead_code, unreachable_pub)]
#[path = "../../ringbuf-core/src/ring.rs"]
mod ring;

mod driver;

use driver::RustRingModule;

module! {
    type: RustRingModule,
    name: "rustring_naive",
    authors: ["Gaurav Gupta", "Sujeet Venkata Raman"],
    description: "Ring-buffer character device (Rust, deliberately unchecked PEEK/RESIZE)",
    license: "GPL",
}

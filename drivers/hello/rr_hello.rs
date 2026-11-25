// SPDX-License-Identifier: GPL-2.0

//! Phase 1 toolchain check: the smallest possible out-of-tree Rust module.
//!
//! If this loads and unloads on the emulated board, the whole pipeline works:
//! rustc + bindgen-generated kernel bindings + Kbuild + modpost + insmod.

use kernel::prelude::*;

module! {
    type: RrHello,
    name: "rr_hello",
    authors: ["Gaurav Gupta", "Sujeet Venkata Raman"],
    description: "Out-of-tree Rust module toolchain check",
    license: "GPL",
}

struct RrHello {
    greeting: KVec<u8>,
}

impl kernel::Module for RrHello {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        // A fallible kernel allocation (kmalloc under the hood). On failure
        // the `?` returns -ENOMEM to insmod instead of crashing.
        let mut greeting = KVec::with_capacity(16, GFP_KERNEL)?;
        greeting.extend_from_slice(b"hello from Rust", GFP_KERNEL)?;
        pr_info!("loaded, buffer holds {} bytes\n", greeting.len());
        Ok(RrHello { greeting })
    }
}

impl Drop for RrHello {
    // Runs on `rmmod`. The KVec is freed automatically right after this
    // function returns -- there is no kfree() to forget.
    fn drop(&mut self) {
        pr_info!("unloading, freeing {} bytes\n", self.greeting.len());
    }
}

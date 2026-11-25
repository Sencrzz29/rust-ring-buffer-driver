// SPDX-License-Identifier: GPL-2.0

//! Positive control for kmemleak: a module that leaks memory in 100% safe
//! Rust.
//!
//! Rust guarantees memory *safety* (no use-after-free, no out-of-bounds, no
//! double free), not freedom from leaks: `core::mem::forget` is a safe
//! function. The hygiene test loads and unloads this module and expects the
//! debug kernel's kmemleak to report the 4 KiB it never frees -- proving
//! that "0 leaks" for the real drivers is a real measurement.

use kernel::prelude::*;

module! {
    type: RrLeak,
    name: "rr_leak",
    authors: ["Gaurav Gupta", "Sujeet Venkata Raman"],
    description: "kmemleak positive control: leaks 4 KiB in safe Rust",
    license: "GPL",
}

struct RrLeak;

impl kernel::Module for RrLeak {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        let buf = KVec::<u8>::from_elem(0x5a, 4096, GFP_KERNEL)?;
        // Not undefined behaviour -- just a leak. No unsafe needed.
        core::mem::forget(buf);
        pr_info!("leaked 4096 bytes on purpose\n");
        Ok(RrLeak)
    }
}

# Results

All numbers come from `scripts/test-all.sh` on Linux 6.18.65 (ARM64, `LLVM=1`,
rustc 1.93.1, clang 21.1.8) running on an emulated QEMU `virt` board
(4× Cortex-A72, 1 GiB) inside WSL2 on a Ryzen 5 7600X. Raw data and logs are
in [`results/`](../results/); this page interprets them.

Modules compared (identical ABI, algorithm, locking and default capacity):

| Module | Description |
|---|---|
| `rustring` | safe Rust, panic-free rules enforced by Clippy deny-lints (the deliverable) |
| `cring` | careful C, every check written by hand |
| `rustring_naive` | Rust with PEEK/RESIZE ported line-by-line from unchecked C (`buf[a..b]`, `+`, `%`, `.unwrap()`) |
| `cring_naive` | C with the PEEK range check and RESIZE bounds/busy checks removed |

---

## 1. Correctness and hygiene (`rustring` and `cring`)

| Check | Kernel | `rustring` | `cring` | Source |
|---|---|---|---|---|
| `ringtest`: 66 functional, hostile-input, bad-pointer and concurrency checks | perf | 66/66 | 66/66 | `functional-perf.txt` |
| same, under KASAN + lockdep + atomic-sleep checking | debug | 66/66, 0 reports | 66/66, 0 reports | `functional-debug.txt` |
| `ringfuzz`: random ops with boundary values and bad pointers | perf | 100 000 ops, 0 violations | 100 000 ops, 0 violations | `fuzz-perf.txt` |
| same, under KASAN + lockdep | debug | 20 000 ops, 0 violations, 0 reports | 20 000 ops, 0 violations, 0 reports | `fuzz-debug.txt` |
| kmemleak after 100 load → use → resize → unload cycles | debug | **0 leaks** | **0 leaks** | `hygiene-debug.txt` |
| "sleeping function called from invalid context" | debug | **0** | — | `hygiene-debug.txt` |

With the same fuzzer seed, both drivers produced **identical** per-operation
histograms of results and error codes: they are behaviourally equivalent, which
is what makes the size and speed comparison fair.

Concurrency coverage inside `ringtest`: a producer/consumer pair streams 8 MiB
with every byte checked in order; 4 writers + 4 readers + an ioctl thread move
8 MiB with every byte read exactly once and the driver's counters matching user
space; CLEAR+RESIZE race three I/O threads and only ever return 0 or `EBUSY`.

### Positive controls — proof that the checkers work

A "0" from a checker only means something if that checker can say "1".

| Control module | Deliberate bug | Compiles? | Functional test | Debug kernel |
|---|---|---|---|---|
| `rr_leak` | `core::mem::forget` of a 4 KiB `KVec` — in **safe** Rust | yes, no warnings | — | kmemleak: **1 unreferenced object (size 4096)** |
| `rustring_atomic` | ring under a `SpinLock`, so `copy_to_user` runs in atomic context | **yes, no warnings** | **passes** (10/10) | **`BUG: sleeping function called from invalid context at lib/iov_iter.c`**, lock `RING` held |

`rustring_atomic` is synopsis problem 3 made concrete: a *protocol* bug that the
Rust type system cannot see, that ordinary testing does not reveal (the user
pages were resident, so nothing actually slept), and that only a run-time
checker reports. It is why `rustring` uses a `Mutex` for the ring.

---

## 2. Crash resilience

Each cell is a fresh VM booted with `oops=panic` (equivalent to
`panic_on_oops=1`, standard on Android/embedded devices: any oops brings the
whole system down). Setup for each attack: a 64-byte ring holding 10 bytes.
Perf-kernel results; the debug kernel gives the same outcomes plus the KASAN
notes shown.

| Attack | `rustring` | `cring` | `rustring_naive` | `cring_naive` |
|---|---|---|---|---|
| PEEK 5 bytes at offset 8 (only 10 stored) | EINVAL | EINVAL | accepted: returns 3 stale bytes | accepted: returns 3 stale bytes |
| PEEK 4096 bytes from the 64-byte buffer | EINVAL | EINVAL | **KERNEL PANIC** — `range end index 4096 out of range for slice of length 64` | **LEAK**: 4096 bytes returned, hundreds of them other kernel heap memory (debug: `BUG: KASAN: slab-out-of-bounds … Read of size 4096`) |
| PEEK offset 2⁶⁴−1 | EINVAL | EINVAL | **KERNEL PANIC** — `range end index 65 out of range` | accepted: offset silently wraps, reads 1 byte past the buffer (debug: KASAN) |
| PEEK length 2⁶⁴−1 | EINVAL | EINVAL | **KERNEL PANIC** — `attempt to add with overflow` | EFAULT, saved by `copy_to_user`'s own `access_ok` check (+ kernel WARNING) |
| RESIZE to 0, then PEEK | EINVAL | EINVAL | **KERNEL PANIC** — `attempt to calculate the remainder with a divisor of zero` | **KERNEL PANIC** — `Unable to handle kernel NULL pointer dereference at virtual address 0000000000000010` |
| RESIZE to 2⁶⁰ bytes | EINVAL | EINVAL | **KERNEL PANIC** — `called Result::unwrap() on an Err value: AllocError` | ENOMEM, but only after a kernel WARNING from `kvmalloc` (> INT_MAX) |
| RESIZE while data is stored | EBUSY | EBUSY | accepted: stored data silently discarded | accepted: stored data silently discarded |
| NULL / kernel / read-only user pointers (18 checks) | all EFAULT | all EFAULT | all EFAULT | all EFAULT |
| 20 000 random operations | PASS, 0 violations | PASS, 0 violations | **KERNEL PANIC** | FAIL, 21 invariant violations (debug: KASAN) |

Kernel-log excerpts: [`results/evidence/`](../results/evidence/).

### What the matrix shows

1. **The two real drivers never fail.** Every hostile input becomes a standard
   errno; the board keeps running and the device stays usable.
2. **Unchecked C fails silently.** Its worst outcome is not the crash but the
   *leak*: a single ioctl hands user space hundreds of bytes of other kernel
   objects (pointers, i.e. KASLR bypass material), with no message at all on a
   production kernel. Only KASAN — never enabled on shipped devices — notices.
3. **Unchecked Rust never corrupts or leaks memory — it panics instead.** Every
   out-of-bounds slice, overflow, division by zero and `unwrap()` was caught,
   but in the kernel a panic is `BUG()`, i.e. a denial of service (synopsis
   problem 2). Without `oops=panic` the caller is killed *while holding the ring
   mutex*, which is never released, so the device hangs forever (Phase 4).
4. **The "Yes+P" rules close that gap.** `rustring` is the same language as
   `rustring_naive`; the only difference is checked arithmetic, `.get()`
   instead of indexing, fallible allocation and no `unwrap` — enforced by
   Clippy so they cannot be forgotten.
5. **Rust does not fix logic bugs.** "PEEK past the data" and "RESIZE while
   busy" are semantic mistakes; both naive builds make them identically. This
   matches the 18.8 % of CVEs that Li et al. (ACSAC 2024) found Rust cannot
   prevent.
6. **Fair to C:** pointer validation is done by the kernel's uaccess layer
   (`copy_to_user`, `access_ok`), so even the naive builds reject bad pointers.

---

## 3. Binary footprint

| Module | `.ko` file | `.text` | `.data` | `.bss` | resident (`/proc/modules`) |
|---|---|---|---|---|---|
| `rustring` | 20 936 B | 4 739 B | 1 088 B | 112 B | 20 480 B |
| `cring` | 14 784 B | 2 891 B | 1 288 B | 64 B | 12 288 B |
| `rustring_naive` | 22 616 B | 5 007 B | 1 088 B | 112 B | 20 480 B |
| `cring_naive` | 14 296 B | 2 733 B | 1 276 B | 64 B | 12 288 B |

* Rust code is **1.64×** the C code (4.7 KB vs 2.9 KB of `.text`); resident
  size is 20 KB vs 12 KB (page-granular). The ARES '19 paper reported 2–3×
  (34 KB vs 10.8 KB) because each module linked its own copy of Rust's `core`;
  in Linux 6.x `core` and the `kernel` crate live in `vmlinux` once, so only
  the driver's own (inlined, monomorphised) code is in the module.
* The checks are cheap: the naive Rust build is *larger* than the checked one
  (5.0 KB vs 4.7 KB) because panic paths carry location data, and the
  checked/naive C difference is 158 bytes.
* Both are "a few tens of kilobytes", as the synopsis expected.

## 4. I/O bandwidth and latency

Perf kernel, 64 KiB ring, 5 runs per point with device order alternated each
run (mean ± standard deviation). The board is emulated (TCG), so absolute
numbers are far below real hardware; the **Rust/C ratio** is the result.

| Workload | Chunk | `rustring` | `cring` | Rust / C |
|---|---|---|---|---|
| write+read, 1 thread | 16 B | 0.7 ± 0.1 MB/s | 0.7 ± 0.2 MB/s | 0.96 |
| write+read, 1 thread | 64 B | 2.8 ± 0.6 MB/s | 2.9 ± 0.6 MB/s | 0.96 |
| write+read, 1 thread | 256 B | 11.5 ± 2.3 MB/s | 10.7 ± 1.6 MB/s | 1.07 |
| write+read, 1 thread | 1 KiB | 44.5 ± 7.6 MB/s | 41.5 ± 3.1 MB/s | 1.07 |
| write+read, 1 thread | 4 KiB | 168.4 ± 29.1 MB/s | 169.9 ± 13.2 MB/s | 0.99 |
| producer/consumer, 2 threads | 512 B | 17.8 ± 3.4 MB/s | 17.9 ± 1.3 MB/s | 0.99 |
| producer/consumer, 2 threads | 4 KiB | 132.0 ± 35.6 MB/s | 128.9 ± 8.1 MB/s | 1.02 |
| latency, 64 B write+read | median | 40.6 µs | 38.6 µs | 1.05 |
| latency, 64 B write+read | 99th pct | 327 µs | 355 µs | 0.92 |

Every ratio lies between 0.92 and 1.07 while the run-to-run spread is 10–20 %:
**no measurable performance difference**. Both drivers do the same work — at
most two copies straight between the ring and the user's buffer per call — and
the bounds checks are a handful of compares next to a system call and a
user-memory copy.

## 5. Static safety audit

| | `ring.rs` | `driver.rs` |
|---|---|---|
| `unsafe` | none — `#![forbid(unsafe_code)]` | 1 `unsafe {}` block + 2 `unsafe(uninit)` declarations, all in the one-time `global_lock!` set-up, each with a `// SAFETY:` comment |
| `buf[i]`, unchecked `+ - * / %`, `unwrap`, `expect`, `panic!` | denied by Clippy | denied by Clippy |
| Proof the gate works | planted `buf[i + j]` → build fails | planted `buf[head + off]` → kernel build fails (`make CLIPPY=1`) |

## 6. Mapping to the synopsis

| Synopsis objective / expected outcome | Evidence |
|---|---|
| Cargo/Kbuild workflow for Linux 6.x on emulated ARM64 | `env/`, `scripts/build-modules.sh`; Phase 1 |
| Character driver with circular ring buffer in safe Rust | `drivers/rust/`, `ringbuf-core/` |
| Panic-free rules (checked math, fallible alloc, `.get()`, `Result`) → errno instead of crash | §2, §5 |
| Locks never forgotten, resources freed by RAII/`Drop` | lock guards; 0 leaks over 100 cycles (§1); lockdep clean |
| Compare with C: footprint, bandwidth, crash resilience | §3, §4, §2 |
| "Zero buffer overflows, UAF or double free in safe code" | none possible in `ring.rs` (no `unsafe`); KASAN clean under fuzzing |
| "Elimination of kernel panics from invalid input" | `rustring`: 0 panics in 9 attacks + 140 000 fuzz ops; contrast `rustring_naive` |
| Near-identical throughput, module under a few tens of KB | ratios 0.92–1.07; 20 KB resident |
| Protocol violations need more than the compiler | `rustring_atomic` control (§1) |

## 7. Limitations

* Emulated board: absolute throughput/latency are not representative of real
  ARM64 hardware; only ratios are meaningful. Real-hardware or x86-64 KVM runs
  would tighten the error bars.
* Single driver type: a virtual character device has no IRQs or DMA, so
  interrupt-context and device-lifetime protocols were not exercised.
* The fuzzer is random, not coverage-guided (syzkaller would go further).
* The out-of-bounds leak and size numbers vary slightly between boots because
  they depend on heap layout and page rounding.

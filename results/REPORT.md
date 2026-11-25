# Short Report: Experimental Results

**Project:** Securing Embedded Linux Device Drivers: A Safe Rust Framework and
Protocol Invariant Implementation
**Team:** Gaurav Gupta (1NT24EC048), Sujeet Venkata Raman (1NT24EC158)
**Platform:** Linux 6.18.65 LTS (`CONFIG_RUST=y`) on an emulated ARM64 board
(QEMU `virt`, 4× Cortex-A72, 1 GiB), built with rustc 1.93.1 and clang 21.

## 1. What was tested

Four loadable kernel modules expose the same ring-buffer character device and
the same ioctl interface:

| Module | Language | Input validation |
|---|---|---|
| `rustring` | Rust | checked arithmetic, `.get()`, fallible allocation, no `unwrap`, enforced by Clippy |
| `cring` | C | the same checks written by hand |
| `rustring_naive` | Rust | checks removed: plain indexing, `+`, `%`, `.unwrap()` |
| `cring_naive` | C | checks removed |

Two kernels were used: a normal one for timing and size, and a debug one with
KASAN (memory errors), lockdep (locking), atomic-sleep detection and kmemleak
(leaks).

## 2. Correctness

| Test | `rustring` | `cring` |
|---|---|---|
| Functional, hostile-input and concurrency suite (66 checks) | 66/66 | 66/66 |
| Same suite on the debug kernel | 66/66, 0 checker reports | 66/66, 0 checker reports |
| Random fuzzing, 100 000 ops (20 000 under KASAN) | 0 violations | 0 violations |
| Memory leaks after 100 load/unload cycles | 0 | 0 |

Both drivers produced identical results for the same fuzzing seed, so they are
functionally equivalent and the comparison below is fair.

**Checker validation:** a module that leaks memory in safe Rust (`rr_leak`) was
reported by kmemleak, and a Rust driver that holds a spinlock while copying
user memory (`rustring_atomic`) compiled without warnings and passed its tests
but was reported by the debug kernel ("sleeping function called from invalid
context"). Both checkers therefore work, and the zeros above are real.

## 3. Crash resilience

Each of 9 attacks was run against each module in a fresh virtual board
configured so that any kernel error brings the whole system down (`oops=panic`).

| Attack | Real drivers (both) | `rustring_naive` | `cring_naive` |
|---|---|---|---|
| Read past the stored data | EINVAL | wrong data returned | wrong data returned |
| Read 4096 bytes from a 64-byte buffer | EINVAL | **kernel panic** | **leaked other kernel memory** |
| Offset 2⁶⁴−1 | EINVAL | **kernel panic** | reads past the buffer |
| Length 2⁶⁴−1 | EINVAL | **kernel panic** | EFAULT + kernel warning |
| Resize to 0, then read | EINVAL | **kernel panic** | **kernel panic** (NULL dereference) |
| Resize to 2⁶⁰ bytes | EINVAL | **kernel panic** | ENOMEM + kernel warning |
| Resize while data is stored | EBUSY | data silently lost | data silently lost |
| Bad pointers (18 cases) | EFAULT | EFAULT | EFAULT |
| 20 000 random operations | pass | **kernel panic** | 21 violations |

**Summary:** the real drivers survived 9/9 attacks. Unchecked Rust crashed the
system 6 times but never leaked or corrupted memory; unchecked C leaked kernel
memory silently (detected only by KASAN, which shipped devices do not run),
returned wrong data and crashed once.

## 4. Binary footprint

| | `rustring` | `cring` | Ratio |
|---|---|---|---|
| Code (`.text`) | 4 739 B | 2 891 B | 1.64× |
| Loaded size (`/proc/modules`) | 20 480 B | 12 288 B | 1.67× |

## 5. Performance

Mean of 5 runs, device order alternated; Rust ÷ C:

| Workload | Rust / C |
|---|---|
| Single-thread write+read, 16 B – 4 KiB chunks | 0.96 – 1.07 |
| Producer/consumer threads, 512 B and 4 KiB | 0.99 – 1.02 |
| 64-byte latency, median / 99th percentile | 1.05 / 0.92 |

All ratios fall within the run-to-run variation (10–20 %): **no measurable
performance difference.** Absolute figures are low because the board is
emulated.

## 6. Conclusions

1. A driver in safe Rust with defensive coding rules turned every invalid input
   into a standard error code, with no crash, leak or checker report.
2. Rust alone prevents memory corruption but not crashes: without the rules,
   each mistake becomes a kernel panic (denial of service).
3. Unchecked C fails silently, including leaking kernel memory to user space.
4. Some bugs remain the programmer's responsibility in any language: logic
   errors, and kernel protocol rules such as not sleeping under a spinlock,
   which only run-time checkers detected.
5. Safety cost no measurable speed; the Rust module is about 1.6× larger but
   still only a few kilobytes of code.

Detailed tables and interpretation: [docs/RESULTS.md](../docs/RESULTS.md).
Raw data: the CSV and text files in this folder; kernel-log excerpts in
[evidence/](evidence/).

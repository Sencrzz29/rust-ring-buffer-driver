# Viva notes

Short answers to the questions most likely to come up, with where to point in
the code. Results referenced here are in [RESULTS.md](RESULTS.md).

## The project in 30 seconds

We wrote a Linux character driver (`/dev/rustring`) in safe Rust on the
kernel's own Rust support (Linux 6.18 LTS), running on an emulated ARM64
board. It keeps a circular byte buffer that programs write to, read from and
inspect with ioctls. We wrote the same driver in C and deliberately broken
("naive") versions of both, then attacked all four. Result: both real drivers
return clean error codes for every attack; naive C silently leaks kernel memory
or crashes; naive Rust never corrupts memory but panics, which in a kernel
takes the board down. The checked Rust driver is as fast as C and 1.6× its code
size.

## Demo script (≈3 minutes)

```bash
wsl -d Ubuntu
cd /mnt/c/Users/sujee/Projects/rust-ringbuf-driver
bash qemu/run.sh
```
```sh
insmod /modules/rustring.ko
echo "hello ring" > /dev/rustring
ringctl /dev/rustring stats
ringctl /dev/rustring peek 0 5; echo
cat /dev/rustring
ringctl /dev/rustring peek 18446744073709551615 2; echo "errno $?"   # 22 = EINVAL
insmod /modules/cring_naive.ko
ringctl /dev/cring_naive resize 64; echo -n 0123456789 > /dev/cring_naive
ringctl /dev/cring_naive peek 0 4096 | od -A d -t x1 | sed -n '5,9p'   # someone else's memory
insmod /modules/rustring_naive.ko
ringctl /dev/rustring_naive resize 64; echo -n 0123456789 > /dev/rustring_naive
ringctl /dev/rustring_naive peek 0 4096      # Rust panic -> kernel BUG -> Oops
dmesg | tail -20
poweroff -f
```

## Likely questions

**Why Rust in the kernel at all? Doesn't the compiler already make it safe?**
The compiler removes memory-safety bugs (overflow, use-after-free, double free)
from *safe* code. It does not remove panics, logic errors or protocol errors.
Our matrix shows all three: `rustring_naive` is memory-safe yet panics the
kernel; both naive builds share the same logic bugs; `rustring_atomic` compiles
cleanly with a sleep-under-spinlock bug.

**What exactly are the "Yes+P" rules and how are they enforced?**
`ringbuf-core/src/ring.rs` top: `#![forbid(unsafe_code)]` and
`#![deny(clippy::indexing_slicing, clippy::arithmetic_side_effects,
clippy::unwrap_used, clippy::expect_used, clippy::panic, …)]`. Kernel builds run
Clippy with `make CLIPPY=1`; a planted `buf[head + off]` makes the build fail.
Every user-derived number goes through `checked_add`/`checked_sub`/`try_from`,
every slice through `.get()`, every allocation is fallible (`?` → `-ENOMEM`).

**Where is `unsafe`, and why is it acceptable?**
One `unsafe {}` block in `RustRingModule::init` (`drivers/rust/driver.rs`) plus
the two `unsafe(uninit)` declarations it pairs with. 6.18's `global_lock!` needs
a one-time initialisation; the safety argument is that it runs once at load,
before `/dev/rustring` exists, so nothing can race it. Everything else is safe
code over the kernel crate, which encapsulates the FFI (the bindgen-generated
`bindings`) — about 9.7 % of the kernel crate is `unsafe` (Li et al. 2024), but
our driver adds essentially none.

**Where did bindgen and the kmalloc wrappers from the synopsis go?**
They are built in to Linux 6.x: the kernel build runs bindgen over the C headers
(`rust/bindings/`), and `KBox`/`KVec`/`KVVec` are safe, fallible, `Drop`-freeing
wrappers over `kmalloc`/`kvmalloc`. The 2019 paper had to write these by hand;
out-of-tree modules now use the upstream ones.

**Why a Mutex and not a SpinLock as the synopsis says?**
`copy_to_user`/`copy_from_user` may page-fault and sleep. Sleeping while holding
a spinlock is a kernel protocol violation. We use a `Mutex` for the ring and a
`SpinLock` for the counters (never held across a sleep). `rustring_atomic.ko`
proves the point: the spinlock version compiles and passes tests, and only the
debug kernel reports `BUG: sleeping function called from invalid context`.

**How do RAII and Drop help?**
The data lives inside the lock (`Mutex<RingState>`), reachable only through the
guard, and the guard unlocks in `Drop` — on every return path, including each
`?`. The buffer is a `KVVec`, freed in `Drop` on resize and on unload; the misc
device deregisters when the module struct is dropped. C's `cring_exit` must call
`misc_deregister` and `kvfree` by hand. Measured: 0 leaks over 100 load/unload
cycles for both — but in C that relies on discipline.
The one place RAII cannot help: statics are never dropped, so the init error
path clears the global ring by hand (commented in `RustRingModule::init`).

**Does Rust prevent memory leaks?**
No. Leaking is safe (`core::mem::forget`). `rr_leak.ko` leaks 4 KiB with no
`unsafe`, and kmemleak catches it — which also proves our "0 leaks" figure is a
real measurement.

**Why is the naive Rust driver "worse" than naive C in some rows?**
It is not worse at safety — it never leaked a byte. It is worse at
*availability*: a Rust panic in the kernel is `BUG()`; with `oops=panic` (as on
phones/IoT) the whole board reboots, and without it the dying task keeps the
ring mutex forever so the device hangs. That is exactly the synopsis's
problem 2, and the reason for the panic-free rules.

**Isn't the C driver just as good if you write it carefully?**
`cring` passes every test too. The difference is who guarantees it: in C every
check and cleanup is the programmer's responsibility and nothing flags an
omission (`cring_naive` compiles without a warning). In Rust, memory safety is
guaranteed and the remaining panic-freedom rules are checked by the build.

**Is the performance comparison fair?**
Same algorithm (two-slice copy straight to/from the user iov), same locks, same
64 KiB ring, same tools, devices alternated every run, 5 runs. Ratios 0.92–1.07
with 10–20 % noise → no measurable difference. Absolute numbers are low because
the ARM64 board is emulated (TCG).

**Why is the Rust module bigger?**
More inlined generic code (`RingBuf<KVVec<u8>>`, iov/uaccess wrappers) and
location data for error paths: 4.7 KB vs 2.9 KB `.text`. Unlike the 2019 paper
(34 KB vs 10.8 KB), `core` is no longer linked into each module.

**Why QEMU and not real hardware / WSL's own kernel?**
WSL's kernel has no Rust support, and the crash tests deliberately kill the
kernel — in a VM that costs one reboot of an emulated board. QEMU `virt` with
Cortex-A72 cores is the standard emulated ARM64 target.

**What would you do next?**
Run on a real board (Raspberry Pi 4) or x86-64 with KVM for absolute numbers;
a driver with IRQs/DMA to exercise interrupt-context protocols; coverage-guided
fuzzing (syzkaller); typestate patterns for driver lifecycles (Cotroneo et al.
2019 suggest runtime monitors; Rust can move some of that to compile time);
klint-style static checking for the atomic-context rule that rustc misses.

## Where things are

| Topic | File |
|---|---|
| Ring logic and all bounds checks | `ringbuf-core/src/ring.rs` |
| Host tests (unit + property) | `ringbuf-core/tests/` |
| Rust driver | `drivers/rust/driver.rs`, crate roots `rustring*.rs` |
| C driver | `drivers/c/cring.c` |
| ioctl ABI | `include/uapi/ringbuf.h` |
| Kernel config | `env/kconfig/*.config` |
| Tests / fuzz / bench | `userspace/ringtest.c`, `ringfuzz.c`, `ringbench.c` |
| Crash matrix | `qemu/tests/attack.sh`, `scripts/crash-matrix.sh` |
| Everything at once | `scripts/test-all.sh` |

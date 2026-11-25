# rust-ringbuf-driver

A panic-resilient Linux character-device driver with a circular ring buffer, written in
safe Rust for Linux 6.18 LTS and run on an emulated ARM64 board (QEMU), compared against
an equivalent C driver for binary size, I/O bandwidth and crash resilience.

Mini project: *Securing Embedded Linux Device Drivers: A Safe Rust Framework and Protocol
Invariant Implementation* — Gaurav Gupta (1NT24EC048), Sujeet Venkata Raman (1NT24EC158).

| Document | Contents |
|---|---|
| [docs/RESULTS.md](docs/RESULTS.md) | all measurements and what they mean |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | build pipeline, driver layers, data structure, locking |
| [docs/VIVA_NOTES.md](docs/VIVA_NOTES.md) | demo script and answers to likely questions |
| [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md) | the original plan and design decisions |
| [results/](results/) | raw CSVs, test output, kernel-log evidence |

## Results at a glance

|  | `rustring` (safe Rust) | `cring` (C) | `rustring_naive` | `cring_naive` |
|---|---|---|---|---|
| Functional + concurrency suite (66 checks) | 66/66 | 66/66 | — | — |
| 9 attacks, `oops=panic` board | **all rejected (errno)** | all rejected | **kernel panic in 6/9** | **kernel-memory leak, wrong data, NULL-deref panic** |
| Random fuzzing (100k ops; 20k under KASAN) | 0 violations | 0 violations | kernel panic | 21 violations |
| kmemleak, 100 load/unload cycles | 0 leaks | 0 leaks | — | — |
| `.text` / resident size | 4.7 KB / 20 KB | 2.9 KB / 12 KB | | |
| Throughput vs C | 0.92–1.07× (within noise) | 1× | | |

Positive controls confirm the checkers: `rr_leak` (a leak in *safe* Rust) is caught by
kmemleak, and `rustring_atomic` (ring under a spinlock — compiles and passes tests) is
caught only by the debug kernel's `sleeping function called from invalid context`.

## Repository layout

```
ringbuf-core/   panic-free ring buffer (no_std, no unsafe) + host unit/property tests
drivers/rust/   rustring driver; same source also builds rustring_naive, rustring_atomic
drivers/c/      cring driver; same source also builds cring_naive
drivers/hello/  rr_hello (toolchain check), rr_leak (kmemleak positive control)
include/uapi/   ringbuf.h: ioctl ABI shared by everything
userspace/      ringctl, ringtest, ringfuzz, ringbench (static ARM64)
env/            toolchain install, source fetch, kernel + BusyBox build, kconfig fragments
qemu/           board init, initramfs packer, run.sh, on-board test scripts (qemu/tests/)
scripts/        build-modules, test-core, test-all, crash-matrix, results.py
results/        committed measurements
```

## Quick start

Everything runs inside WSL (`wsl -d Ubuntu`), from the repo folder:

```bash
cd /mnt/c/Users/sujee/Projects/rust-ringbuf-driver
```

One-time setup (Ubuntu 26.04; already done on this machine):

```bash
sudo bash env/00-install-packages.sh     # clang/LLD, rustc + rust-src + bindgen, QEMU (~2.5 GB)
bash env/01-fetch-sources.sh             # Linux 6.18.65 + BusyBox 1.37, checksum-verified
bash env/02-build-kernel.sh perf         # benchmark kernel     (~15 min)
bash env/02-build-kernel.sh debug        # KASAN/lockdep kernel (~15 min)
bash env/03-build-busybox.sh             # static ARM64 BusyBox
```

Reproduce every result (≈1 hour; writes `results/`):

```bash
bash scripts/test-all.sh                 # or a subset: build functional fuzz hygiene crash sizes bench
```

Interactive board:

```bash
bash scripts/build-modules.sh perf && make -C userspace
bash qemu/run.sh                         # -k debug for the KASAN/lockdep kernel
```

```sh
insmod /modules/rustring.ko
echo "hello ring" > /dev/rustring
ringctl /dev/rustring stats              # capacity / used / counters
ringctl /dev/rustring peek 0 5           # look without consuming
cat /dev/rustring                        # drain
ringctl /dev/rustring peek 18446744073709551615 2; echo $?   # 22 = EINVAL, no crash
rmmod rustring
poweroff -f                              # or Ctrl-A X to quit QEMU
```

Single tests unattended: `bash qemu/run.sh -r smoke_all.sh`, or
`bash qemu/run.sh -r oob_peek.sh -x "rustring cring cring_naive rustring_naive"`.

Kernel-side lint gate (the build fails on `buf[i]`, unchecked arithmetic, `unwrap`…):

```bash
source env/common.sh
make -C $KBUILD_PERF M=$PWD/drivers/rust MO=$WORK/modules/perf/rust LLVM=1 ARCH=arm64 CLIPPY=1 modules
```

Build artefacts live in `~/rr` inside WSL (on the external drive), never in this repo.
Run `wsl --shutdown` before unplugging the external drive.

## Toolchain

Linux 6.18.65 LTS · rustc 1.93.1 · bindgen 0.72.1 · clang/LLD 21.1.8 · QEMU 10.2.1 ·
BusyBox 1.37.0 · aarch64-linux-gnu-gcc 15.2 · WSL2 Ubuntu 26.04 LTS.

## License

GPL-2.0 (kernel modules and tools), matching the Linux kernel.

# rust-ringbuf-driver

A panic-resilient Linux character-device driver with a circular ring buffer, written in
safe Rust for Linux 6.18 LTS and run on an emulated ARM64 board (QEMU), compared against
an equivalent C driver for binary size, I/O bandwidth and crash resilience.

Mini project: *Securing Embedded Linux Device Drivers: A Safe Rust Framework and Protocol
Invariant Implementation* — Gaurav Gupta (1NT24EC048), Sujeet Venkata Raman (1NT24EC158).

Design and phases: [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md).

## Status

| Phase | State |
|---|---|
| 0 Environment (WSL Ubuntu 26.04, LLVM 21, rustc 1.93, QEMU 10.2) | done |
| 1 Linux 6.18.65 ARM64 kernels with `CONFIG_RUST=y`, BusyBox initramfs, QEMU board | done (perf kernel; debug kernel builds with the same script) |
| 2 Panic-free ring-buffer core (`ringbuf-core/`) + host tests | done |
| 3 Rust driver `/dev/rustring` (+ `ringctl` tool, smoke test: 16/16 on the board) | done |
| 4 C driver `/dev/cring` + deliberately naive builds of both (`*_naive`) | done |
| 5 Tests, fuzzing, benchmarks | next |
| 6 Results & documentation | — |

## Quick start

Everything runs inside WSL (`wsl -d Ubuntu`), from the repo folder:

```bash
cd /mnt/c/Users/sujee/Projects/rust-ringbuf-driver
```

One-time setup (already done on this machine):

```bash
sudo bash env/00-install-packages.sh     # compilers, Rust, QEMU (~2.5 GB)
bash env/01-fetch-sources.sh             # Linux 6.18.65 + BusyBox, checksum-verified
bash env/02-build-kernel.sh perf         # benchmark kernel   (~15 min)
bash env/02-build-kernel.sh debug        # KASAN/lockdep kernel (~25 min)
bash env/03-build-busybox.sh             # static ARM64 BusyBox
```

Build the modules and tools, then boot the board:

```bash
bash scripts/build-modules.sh perf       # every drivers/*/ against the perf kernel
make -C userspace                        # static ARM64 tools -> /tests on the board
bash qemu/run.sh                         # interactive shell on the emulated board
bash qemu/run.sh -r smoke_all.sh         # run a test unattended and power off
bash qemu/run.sh -k debug -r smoke_all.sh            # same, on the KASAN/lockdep kernel
bash qemu/run.sh -r oob_peek.sh -x "rustring cring cring_naive rustring_naive"
```

The four modules (same ABI, `include/uapi/ringbuf.h`):

| Module | What it is |
|---|---|
| `rustring.ko` | the deliverable: safe Rust, lint-enforced panic-free rules |
| `cring.ko` | the same driver in careful C (fair baseline for size/speed) |
| `rustring_naive.ko` | `--cfg naive`: PEEK/RESIZE ported from unchecked C (`buf[a..b]`, `+`, `.unwrap()`) |
| `cring_naive.ko` | `-DCRING_NAIVE`: PEEK/RESIZE without range/busy checks |

Preview of the out-of-bounds PEEK attack (64-byte buffer, ask for 4096 bytes):

| Module | Result | Afterwards |
|---|---|---|
| `rustring` | `EINVAL` | usable |
| `cring` | `EINVAL` | usable |
| `cring_naive` | returns 4096 bytes, ~570 of them **other kernel heap memory**; silent on the perf kernel, `BUG: KASAN: slab-out-of-bounds` on the debug kernel | usable (the leak goes unnoticed) |
| `rustring_naive` | Rust refuses the out-of-bounds read by **panicking** → `kernel BUG` → Oops, caller killed | **hung**: the dead task still holds the ring mutex |

On the board (`/tests` is on `PATH`):

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

Kernel-side lint gate (fails the build on `buf[i]`, unchecked arithmetic, `unwrap`...):

```bash
source env/common.sh
make -C $KBUILD_PERF M=$PWD/drivers/rust MO=$WORK/modules/perf/rust LLVM=1 ARCH=arm64 CLIPPY=1 modules
```

Host-side tests of the ring-buffer logic (no kernel needed):

```bash
bash scripts/test-core.sh                # clippy (panic-free lints) + unit + property tests
```

Build artefacts live in `~/rr` inside WSL (on the external drive), never in this repo.
Run `wsl --shutdown` before unplugging the external drive.

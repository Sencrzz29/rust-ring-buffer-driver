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
| 3 Rust driver `/dev/rustring` | next |
| 4 C driver + naive variants | — |
| 5 Tests, fuzzing, benchmarks | — |
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

Build the modules and boot the board:

```bash
bash scripts/build-modules.sh perf       # every drivers/*/ against the perf kernel
bash qemu/run.sh                         # interactive shell on the emulated board
#   rr-board# insmod /modules/rr_hello.ko ; dmesg | tail ; rmmod rr_hello
#   rr-board# poweroff -f                # (or Ctrl-A X) to quit QEMU
bash qemu/run.sh -r hello.sh             # run a test unattended and power off
bash qemu/run.sh -k debug                # same, on the debug kernel
```

Host-side tests of the ring-buffer logic (no kernel needed):

```bash
bash scripts/test-core.sh                # clippy (panic-free lints) + unit + property tests
```

Build artefacts live in `~/rr` inside WSL (on the external drive), never in this repo.
Run `wsl --shutdown` before unplugging the external drive.

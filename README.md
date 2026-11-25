# rust-ringbuf-driver

A panic-resilient Linux character-device driver with a circular ring buffer, written in
safe Rust for Linux 6.18 LTS and run on an emulated ARM64 board (QEMU), compared against
an equivalent C driver for binary size, I/O bandwidth and crash resilience.

Mini project: *Securing Embedded Linux Device Drivers: A Safe Rust Framework and Protocol
Invariant Implementation* — Gaurav Gupta (1NT24EC048), Sujeet Venkata Raman (1NT24EC158).

**Status:** planning. See [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md).

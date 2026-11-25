#!/usr/bin/env bash
# Install everything needed to build Linux 6.18 with Rust support for ARM64,
# cross-compile user-space test programs, and run the result under QEMU.
# Target host: Ubuntu 26.04 LTS (WSL2). Run as root:
#   wsl -d Ubuntu -u root -- bash env/00-install-packages.sh
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "run as root (sudo bash $0)" >&2
    exit 1
fi

export DEBIAN_FRONTEND=noninteractive

PKGS=(
    # generic build tooling the kernel's Makefiles expect
    build-essential make flex bison bc cpio kmod rsync xz-utils bzip2 file
    libelf-dev libssl-dev python3 ca-certificates
    # LLVM/Clang toolchain: Rust-for-Linux requires the C side to be built
    # with clang so C and Rust objects share one ABI/codegen backend
    clang lld llvm libclang-dev
    # Rust toolchain from Ubuntu (kernel docs: works out of the box on >=25.04)
    rustc rust-src cargo bindgen rustfmt rust-clippy
    # ARM64 cross C compiler + libc, for BusyBox and the static test binaries
    gcc-aarch64-linux-gnu libc6-dev-arm64-cross
    # the emulated board and a debugger that understands ARM64
    qemu-system-arm gdb-multiarch
)

apt-get update
apt-get install -y --no-install-recommends "${PKGS[@]}"

echo
echo "== installed versions =="
clang --version | head -1
ld.lld --version | head -1
rustc --version
bindgen --version
qemu-system-aarch64 --version | head -1
aarch64-linux-gnu-gcc --version | head -1

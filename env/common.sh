# Shared settings for all build/run scripts. Source it, don't execute it:
#   source env/common.sh
#
# Layout inside WSL (ext4, NOT /mnt/*):
#   $WORK/dl                 downloaded tarballs + checksum files
#   $WORK/src/linux-X.Y.Z    pristine kernel source
#   $WORK/build-perf         kernel build output, benchmark config
#   $WORK/build-debug        kernel build output, KASAN/lockdep/kmemleak config
#   $WORK/busybox-X.Y.Z      BusyBox source + build
#   $WORK/rootfs-base        BusyBox installed tree (input to initramfs)
#   $WORK/initramfs          generated initramfs images

_rr_env_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=versions.env
source "$_rr_env_dir/versions.env"

export REPO=${REPO:-$(cd "$_rr_env_dir/.." && pwd)}
export WORK=${WORK:-$HOME/rr}
export KSRC=$WORK/src/linux-$KERNEL_VERSION
export KBUILD_PERF=$WORK/build-perf
export KBUILD_DEBUG=$WORK/build-debug
export BUSYBOX_SRC=$WORK/busybox-$BUSYBOX_VERSION

# Cross-build for the emulated ARM64 board with the LLVM toolchain.
export ARCH=arm64
export LLVM=1
export CROSS_COMPILE_USER=aarch64-linux-gnu-   # for user-space C (BusyBox, tests)

# Ubuntu's rust-src package: tell Kbuild where the `core` library sources are.
RUST_LIB_SRC=/usr/src/rustc-$(rustc --version | cut -d' ' -f2)/library
export RUST_LIB_SRC

export JOBS=${JOBS:-$(nproc)}

# kbuild_dir perf|debug -> path of that kernel build tree
kbuild_dir() {
    case "$1" in
        perf)  echo "$KBUILD_PERF" ;;
        debug) echo "$KBUILD_DEBUG" ;;
        *) echo "unknown kernel flavour '$1' (want perf|debug)" >&2; return 1 ;;
    esac
}

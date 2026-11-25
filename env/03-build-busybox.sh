#!/usr/bin/env bash
# Build a static ARM64 BusyBox (shell + coreutils for the emulated board)
# and install it into $WORK/rootfs-base.
set -euo pipefail
source "$(dirname "$0")/common.sh"

cd "$BUSYBOX_SRC"
bb() { make CROSS_COMPILE="$CROSS_COMPILE_USER" "$@"; }

if [[ ! -f .config || ${RECONFIG:-0} == 1 ]]; then
    bb defconfig
    # Static binary: the initramfs has no shared libraries.
    sed -i 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' .config
    # `tc` uses traffic-control definitions removed from modern kernel
    # headers (CBQ); we don't need it.
    sed -i 's/^CONFIG_TC=y$/# CONFIG_TC is not set/' .config
    # BusyBox 1.37 bug: the SHA "hardware acceleration" code references
    # x86-only (SHA-NI) functions even when cross-building for ARM64.
    sed -i -e 's/^CONFIG_SHA1_HWACCEL=y$/# CONFIG_SHA1_HWACCEL is not set/' \
           -e 's/^CONFIG_SHA256_HWACCEL=y$/# CONFIG_SHA256_HWACCEL is not set/' .config
    # GCC 15 defaults to C23; BusyBox 1.37 predates it.
    sed -i 's/^CONFIG_EXTRA_CFLAGS=.*/CONFIG_EXTRA_CFLAGS="-std=gnu17"/' .config
    bb oldconfig </dev/null >/dev/null
fi
grep -qx 'CONFIG_STATIC=y' .config

bb -j"$JOBS"
rm -rf "$WORK/rootfs-base"
bb CONFIG_PREFIX="$WORK/rootfs-base" install >/dev/null
file "$WORK/rootfs-base/bin/busybox"

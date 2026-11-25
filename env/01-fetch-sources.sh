#!/usr/bin/env bash
# Download the Linux kernel and BusyBox sources, verify SHA-256 checksums
# published by upstream (over HTTPS), and unpack them under $WORK.
set -euo pipefail
source "$(dirname "$0")/common.sh"

mkdir -p "$WORK/dl" "$WORK/src"
cd "$WORK/dl"

fetch() { # url dest
    if [[ ! -s $2 ]]; then
        echo "downloading $1"
        curl -fL --retry 3 --progress-bar -o "$2.part" "$1"
        mv "$2.part" "$2"
    fi
}

# --- Linux -----------------------------------------------------------------
major=${KERNEL_VERSION%%.*}
ktar=linux-$KERNEL_VERSION.tar.xz
fetch "https://cdn.kernel.org/pub/linux/kernel/v$major.x/$ktar" "$ktar"
curl -fsSL -o sha256sums.asc "https://cdn.kernel.org/pub/linux/kernel/v$major.x/sha256sums.asc"
grep -E "  $ktar\$" sha256sums.asc | sha256sum -c -

if [[ ! -d $KSRC ]]; then
    echo "unpacking $ktar"
    tar -C "$WORK/src" -xf "$ktar"
fi

# --- BusyBox ---------------------------------------------------------------
btar=busybox-$BUSYBOX_VERSION.tar.bz2
fetch "https://busybox.net/downloads/$btar" "$btar"
fetch "https://busybox.net/downloads/$btar.sha256" "$btar.sha256"
sha256sum -c "$btar.sha256"

if [[ ! -d $BUSYBOX_SRC ]]; then
    echo "unpacking $btar"
    tar -C "$WORK" -xf "$btar"
fi

echo "sources ready: $KSRC  $BUSYBOX_SRC"

#!/usr/bin/env bash
# Pack the board's root filesystem into an initramfs (gzip'd cpio):
#   BusyBox + qemu/init + modules for <flavour> + test programs/scripts.
#   qemu/mkinitramfs.sh perf|debug        -> $WORK/initramfs/<flavour>.cpio.gz
#
# Uses the kernel's own gen_init_cpio, which can create /dev/console without
# root privileges (the kernel needs it before devtmpfs is mounted).
set -euo pipefail
source "$(dirname "$0")/../env/common.sh"

flavour=${1:?usage: $0 perf|debug}
kdir=$(kbuild_dir "$flavour")
stage=$WORK/initramfs/stage-$flavour
outimg=$WORK/initramfs/$flavour.cpio.gz

rm -rf "$stage"
mkdir -p "$stage"/{proc,sys,dev,tmp,etc,root,modules,tests}
cp -a "$WORK/rootfs-base/." "$stage/"
install -m 755 "$REPO/qemu/init" "$stage/init"

# kernel modules built for this flavour
shopt -s nullglob
for ko in "$WORK/modules/$flavour"/*.ko; do install -m 644 "$ko" "$stage/modules/"; done
# test scripts and cross-compiled test binaries
for t in "$REPO"/qemu/tests/*.sh "$REPO"/userspace/build/*; do install -m 755 "$t" "$stage/tests/"; done
shopt -u nullglob

spec=$WORK/initramfs/$flavour.spec
{
    echo "nod /dev/console 600 0 0 c 5 1"
    echo "nod /dev/null 666 0 0 c 1 3"
    (cd "$stage" && find . -mindepth 1 | LC_ALL=C sort) | while IFS= read -r p; do
        rel=${p#.}
        if   [[ -L $stage$rel ]]; then echo "slink $rel $(readlink "$stage$rel") 777 0 0"
        elif [[ -d $stage$rel ]]; then echo "dir $rel 755 0 0"
        elif [[ -f $stage$rel ]]; then echo "file $rel $stage$rel $(stat -c %a "$stage$rel") 0 0"
        fi
    done
} > "$spec"

"$kdir/usr/gen_init_cpio" "$spec" | gzip -9 > "$outimg"
echo "initramfs: $outimg ($(du -h "$outimg" | cut -f1)), modules: $(ls "$stage/modules" | tr '\n' ' ')"

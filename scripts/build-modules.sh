#!/usr/bin/env bash
# Build every out-of-tree module under drivers/ against one kernel flavour.
#   scripts/build-modules.sh perf|debug [extra make args, e.g. CLIPPY=1]
#
# Sources stay in the repo (drivers/<name>/); object files go to
# $WORK/modules/<flavour>/<name>/ via Kbuild's MO= option, so a perf and a
# debug build never clobber each other and no build junk lands in the repo.
set -euo pipefail
source "$(dirname "$0")/../env/common.sh"

flavour=${1:?usage: $0 perf|debug [make args]}
shift
kdir=$(kbuild_dir "$flavour")
[[ -f $kdir/Module.symvers ]] || { echo "kernel '$flavour' not built yet ($kdir)" >&2; exit 1; }

out=$WORK/modules/$flavour
mkdir -p "$out"

for d in "$REPO"/drivers/*/; do
    [[ -f $d/Kbuild ]] || continue
    name=$(basename "$d")
    mkdir -p "$out/$name"
    echo "== $name ($flavour)"
    make -C "$kdir" M="$d" MO="$out/$name" ARCH=arm64 LLVM=1 -j"$JOBS" "$@" modules
    cp "$out/$name"/*.ko "$out/"
done

ls -l "$out"/*.ko

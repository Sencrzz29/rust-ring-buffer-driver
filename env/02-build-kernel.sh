#!/usr/bin/env bash
# Configure and build an ARM64 kernel with Rust support.
#   env/02-build-kernel.sh perf     # benchmark kernel
#   env/02-build-kernel.sh debug    # KASAN + lockdep + kmemleak kernel
# Set RECONFIG=1 to regenerate .config from scratch.
set -euo pipefail
source "$(dirname "$0")/common.sh"

flavour=${1:?usage: $0 perf|debug}
out=$(kbuild_dir "$flavour")
mkdir -p "$out"

kmake() { make -C "$KSRC" O="$out" ARCH=arm64 LLVM=1 "$@"; }

frags=("$REPO/env/kconfig/common.config" "$REPO/env/kconfig/$flavour.config")

# Every option we request must survive Kconfig dependency resolution;
# Kconfig silently drops options whose dependencies aren't met.
verify_config() {
    local bad=0 line opt val
    while IFS= read -r line; do
        if [[ $line =~ ^(CONFIG_[A-Za-z0-9_]+)=(.*)$ ]]; then
            opt=${BASH_REMATCH[1]} val=${BASH_REMATCH[2]}
            if ! grep -qxF "$opt=$val" "$out/.config"; then
                echo "config mismatch: wanted $opt=$val, got: $(grep -E "^(# )?$opt[= ]" "$out/.config" || echo absent)"
                bad=1
            fi
        elif [[ $line =~ ^#\ (CONFIG_[A-Za-z0-9_]+)\ is\ not\ set$ ]]; then
            opt=${BASH_REMATCH[1]}
            if grep -qE "^$opt=" "$out/.config"; then
                echo "config mismatch: wanted $opt unset, got: $(grep -E "^$opt=" "$out/.config")"
                bad=1
            fi
        fi
    done < <(cat "${frags[@]}")
    return $bad
}

if [[ ! -f $out/.config || ${RECONFIG:-0} == 1 ]]; then
    kmake defconfig
    kmake mod2noconfig          # drop the ~1000 defconfig modules we don't need
    "$KSRC/scripts/kconfig/merge_config.sh" -m -O "$out" "$out/.config" "${frags[@]}"
    kmake olddefconfig
fi
verify_config
echo "config OK ($flavour)"
[[ ${CONFIG_ONLY:-0} == 1 ]] && exit 0

time kmake -j"$JOBS" Image modules
ls -l "$out/arch/arm64/boot/Image" "$out/Module.symvers"
echo "kernel built: $(cat "$out/include/config/kernel.release")"

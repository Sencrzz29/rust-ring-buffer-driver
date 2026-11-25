#!/usr/bin/env bash
# Crash-resilience matrix: every attack scenario x every module, each in a
# fresh VM booted with oops=panic (panic_on_oops; as on Android / embedded boards, an
# oops takes the whole system down).
#   scripts/crash-matrix.sh [perf|debug] [parallel VMs]
# Output: results/crash-matrix-<flavour>.{csv,md}; raw logs in $WORK/logs.
set -euo pipefail
source "$(dirname "$0")/../env/common.sh"

flavour=${1:-perf}
jobs=${2:-3}
export flavour
export logdir=$WORK/logs/crash-$flavour
export tmo=$([[ $flavour == debug ]] && echo 300 || echo 120)
rm -rf "$logdir"
mkdir -p "$logdir" "$REPO/results"

"$REPO/qemu/mkinitramfs.sh" "$flavour" > /dev/null

modules="rustring cring rustring_naive cring_naive"
scenarios="peek_past_data peek_oob peek_off_overflow peek_len_overflow resize_zero resize_huge resize_busy bad_pointers fuzz"

cell() {
    bash "$REPO/qemu/run.sh" -n -k "$flavour" -r attack.sh -x "$1 $2" \
        -a "oops=panic" -t "$tmo" -l "$logdir/$1.$2.log" > /dev/null 2>&1 || true
    echo "  done $1 / $2"
}
export -f cell

for s in $scenarios; do for m in $modules; do echo "$m $s"; done; done |
    xargs -P "$jobs" -L 1 bash -c 'cell "$0" "$1"'

python3 "$REPO/scripts/results.py" crash "$logdir" "$REPO/results/crash-matrix-$flavour"

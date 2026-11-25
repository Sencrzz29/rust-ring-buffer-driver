#!/usr/bin/env bash
# Phase 5: build everything, run every test, write results/.
#   scripts/test-all.sh [step...]
# Steps (default: all, in this order):
#   build functional fuzz hygiene crash sizes bench
# Raw serial logs go to $WORK/logs; curated results to results/.
set -euo pipefail
source "$(dirname "$0")/../env/common.sh"

steps=("$@")
[[ ${#steps[@]} -eq 0 ]] && steps=(build functional fuzz hygiene crash sizes bench)
R=$REPO/results
L=$WORK/logs
mkdir -p "$R" "$L"

# board <flavour> <test> "<args>" <logname> [timeout]  -> log text on stdout (CR stripped)
board() {
    bash "$REPO/qemu/run.sh" -k "$1" -r "$2" -x "$3" -t "${5:-1800}" -l "$L/$4.log" > /dev/null 2>&1 || true
    tr -d '\r' < "$L/$4.log"
}

for step in "${steps[@]}"; do
    echo "===== $step"
    case $step in
    build)
        bash "$REPO/scripts/test-core.sh"
        bash "$REPO/scripts/build-modules.sh" perf > /dev/null
        bash "$REPO/scripts/build-modules.sh" debug > /dev/null
        make -s -C "$REPO/userspace"
        ;;
    functional)
        for k in perf debug; do
            board $k functional.sh "rustring cring" functional-$k |
                grep -aE '^(ok|not ok|# )|SUITE|SPLATS|^\[.*(BUG|WARNING)' > "$R/functional-$k.txt"
            grep -E 'SUITE|SPLATS|# ringtest' "$R/functional-$k.txt"
        done
        ;;
    fuzz)
        board perf fuzz.sh "100000 1 rustring cring" fuzz-perf |
            grep -aE '^# |FUZZ|SPLATS|VIOLATION' > "$R/fuzz-perf.txt"
        board debug fuzz.sh "20000 2 rustring cring" fuzz-debug 3600 |
            grep -aE '^# |FUZZ|SPLATS|VIOLATION' > "$R/fuzz-debug.txt"
        grep -hE 'FUZZ|SPLATS|ringfuzz .*:' "$R"/fuzz-*.txt
        ;;
    hygiene)
        board debug hygiene.sh "100" hygiene-debug 3600 |
            grep -aE '^(KMEMLEAK|ATOMIC|  \|)' > "$R/hygiene-debug.txt"
        grep -E '^(KMEMLEAK|ATOMIC)' "$R/hygiene-debug.txt"
        ;;
    crash)
        bash "$REPO/scripts/crash-matrix.sh" perf 3
        bash "$REPO/scripts/crash-matrix.sh" debug 3
        ;;
    sizes)
        board perf modsizes.sh "" modsizes > /dev/null
        python3 "$REPO/scripts/results.py" sizes "$WORK/modules/perf" "$L/modsizes.log" "$R/sizes"
        ;;
    bench)
        board perf bench.sh "-n 20000 -r 5 -b 16 -L 20000" bench 3600 > /dev/null
        python3 "$REPO/scripts/results.py" bench "$L/bench.log" "$R/bench"
        ;;
    *) echo "unknown step $step" >&2; exit 2 ;;
    esac
done

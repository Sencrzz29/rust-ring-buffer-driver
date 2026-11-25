#!/bin/sh
# Random-operation fuzzing of each module.
#   sh /tests/fuzz.sh <iterations> <seed> <module> [module...]
iters=$1 seed=$2
shift 2
rc=0
for mod in "$@"; do
    insmod "/modules/$mod.ko" || { echo "FUZZ[$mod]: insmod failed"; rc=1; continue; }
    dmesg -c > /dev/null
    if ringfuzz "/dev/$mod" "$iters" "$seed"; then
        echo "FUZZ[$mod]: PASS"
    else
        echo "FUZZ[$mod]: FAIL"; rc=1
    fi
    splats=$(dmesg | grep -cE 'BUG:|WARNING:|Oops|possible circular|sleeping function called|KASAN')
    echo "SPLATS[$mod]: $splats"
    [ "$splats" -eq 0 ] || { dmesg | grep -E -A3 'BUG:|WARNING:|Oops|circular|sleeping|KASAN' | head -20; rc=1; }
    rmmod "$mod"
done
exit $rc

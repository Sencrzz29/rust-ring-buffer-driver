#!/bin/sh
# Full functional / adversarial / concurrency suite (ringtest) for each module.
#   sh /tests/functional.sh [--quick] <module> [module...]
quick=
[ "$1" = "--quick" ] && { quick=--quick; shift; }
rc=0
for mod in "$@"; do
    insmod "/modules/$mod.ko" || { echo "SUITE[$mod]: insmod failed"; rc=1; continue; }
    dmesg -c > /dev/null
    if ringtest "/dev/$mod" $quick; then
        echo "SUITE[$mod]: PASS"
    else
        echo "SUITE[$mod]: FAIL"; rc=1
    fi
    splats=$(dmesg | grep -cE 'BUG:|WARNING:|Oops|possible circular|sleeping function called|KASAN')
    echo "SPLATS[$mod]: $splats"
    [ "$splats" -eq 0 ] || { dmesg | grep -E -A3 'BUG:|WARNING:|Oops|circular|sleeping|KASAN' | head -20; rc=1; }
    rmmod "$mod"
done
exit $rc

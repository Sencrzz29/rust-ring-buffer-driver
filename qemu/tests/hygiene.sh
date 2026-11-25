#!/bin/sh
# Debug-kernel hygiene checks, each with a positive control that proves the
# checker would have fired:
#   kmemleak       N load/use/unload cycles of rustring and cring -> 0 leaks
#                  control: rr_leak (leaks 4 KiB in safe Rust) -> >= 1 leak
#   atomic sleep   smoke workload on rustring -> 0 reports
#                  control: rustring_atomic (ring under a spinlock) -> >= 1
#
#   sh /tests/hygiene.sh [cycles]
KML=/sys/kernel/debug/kmemleak
cycles=${1:-100}

[ -e $KML ] || { echo "HYGIENE: kmemleak not available (not the debug kernel?)"; exit 1; }

# kmemleak only reports objects older than 5 s, and needs two scans to be sure.
scan() {
    sleep 6
    echo scan > $KML
    echo scan > $KML
    grep -c "unreferenced object" $KML
}

# Absorb anything reported from boot so only new leaks count.
echo scan > $KML
echo clear > $KML

for mod in rustring cring; do
    dev=/dev/$mod
    i=0
    while [ $i -lt "$cycles" ]; do
        insmod "/modules/$mod.ko"
        printf 'some data' > $dev
        ringctl $dev peek 0 4 > /dev/null
        cat $dev > /dev/null
        ringctl $dev resize 4096
        printf 'more data' > $dev
        ringctl $dev resize 128 2>/dev/null   # EBUSY path frees the new buffer
        rmmod "$mod"
        i=$((i + 1))
    done
    echo "KMEMLEAK[$mod]: cycles=$cycles leaks=$(scan)"
    echo clear > $KML
done

insmod /modules/rr_leak.ko
rmmod rr_leak
n=$(scan)
echo "KMEMLEAK[rr_leak]: leaks=$n (positive control, expected >= 1)"
grep -m1 -A14 "unreferenced object" $KML | sed 's/^/  | /'
echo clear > $KML

dmesg -c > /dev/null
sh /tests/ring_smoke.sh rustring full > /dev/null 2>&1
echo "ATOMIC[rustring]: sleep-in-atomic reports=$(dmesg | grep -c 'sleeping function called from invalid context')"

dmesg -c > /dev/null
sh /tests/ring_smoke.sh rustring_atomic benign > /tmp/smoke 2>&1
echo "ATOMIC[rustring_atomic]: functional smoke $(tail -1 /tmp/smoke)"
echo "ATOMIC[rustring_atomic]: sleep-in-atomic reports=$(dmesg | grep -c 'sleeping function called from invalid context') (positive control, expected >= 1)"
dmesg | grep -m1 -A16 'sleeping function called' | sed 's/^/  | /'

#!/bin/sh
# One cell of the crash-resilience matrix: one attack against one module, in
# a fresh VM (the host runs every cell with oops=panic, i.e. panic_on_oops).
#   sh /tests/attack.sh <module> <scenario>
#
# Setup: 64-byte ring holding "0123456789".
mod=$1 sc=$2 dev=/dev/$1

insmod "/modules/$mod.ko" || { echo "CELL[$mod/$sc]: insmod-failed"; exit 1; }
ringctl "$dev" resize 64 > /dev/null 2>&1
printf '0123456789' > "$dev"
: > /tmp/out
dmesg -c > /dev/null
echo "ATTACK[$mod/$sc]: start"

case $sc in
    peek_past_data)     ringctl "$dev" peek 8 5 > /tmp/out ;;                     # inside buffer, past data
    peek_oob)           ringctl "$dev" peek 0 4096 > /tmp/out ;;                  # past the 64-byte buffer
    peek_off_overflow)  ringctl "$dev" peek 18446744073709551615 2 > /tmp/out ;;  # offset + head wraps
    peek_len_overflow)  ringctl "$dev" peek 2 18446744073709551615 > /tmp/out ;;  # offset + len wraps
    resize_zero)        ringctl "$dev" clear; ringctl "$dev" resize 0 && ringctl "$dev" peek 0 1 > /tmp/out ;;
    resize_huge)        ringctl "$dev" clear; ringctl "$dev" resize 1152921504606846976 ;;
    resize_busy)        ringctl "$dev" resize 128 ;;                              # data present
    bad_pointers)       ringtest "$dev" --only badptr > /tmp/out ;;
    fuzz)               ringfuzz "$dev" 20000 7 > /tmp/out ;;
    *)                  echo "unknown scenario $sc"; exit 2 ;;
esac
rc=$?

n=$(wc -c < /tmp/out)
nz=$(tr -d '\000' < /tmp/out | wc -c)
# Is the device still usable? (a task killed while holding the ring mutex
# leaves it locked forever; mutex_lock() is not interruptible)
( ringctl "$dev" stats > /tmp/st 2>&1 ) &
pid=$!
sleep 2
if kill -0 $pid 2>/dev/null; then state=hung; st=; else state=usable; st=$(head -1 /tmp/st); fi
[ "$sc" = fuzz ] || [ "$sc" = bad_pointers ] && st="$st | $(tail -1 /tmp/out)"

echo "CELL[$mod/$sc]: rc=$rc out_bytes=$n out_nonzero=$nz device=$state stats=$st"
echo "SPLAT[$mod/$sc]: kasan=$(dmesg | grep -c 'BUG: KASAN') warn=$(dmesg | grep -c 'WARNING:') oops=$(dmesg | grep -c 'Internal error\|Unable to handle') sleep=$(dmesg | grep -c 'sleeping function called')"
dmesg | grep -m3 -E 'BUG: KASAN|WARNING:|Read of size' | sed 's/^/  | /'

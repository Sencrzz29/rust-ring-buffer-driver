#!/bin/sh
# The classic driver bug, as an attack: ask PEEK for far more bytes than the
# buffer holds. Runs against each module given on the command line, in turn.
#
#   sh /tests/oob_peek.sh <module> [module...]
#
# Setup per module: 64-byte buffer holding "0123456789", then
#   PEEK offset=0 len=4096   (user buffer is 4096 bytes, so the copy itself
#                             is legal -- only the driver can say no)
# Reports what came back, and whether the device still works afterwards.

for mod in "$@"; do
    dev=/dev/$mod
    echo "== $mod"
    insmod "/modules/$mod.ko" || { echo "result[$mod]: insmod failed"; continue; }
    ringctl "$dev" resize 64
    printf '0123456789' > "$dev"
    dmesg -c > /dev/null

    ringctl "$dev" peek 0 4096 > /tmp/out 2>/tmp/err
    rc=$?
    n=$(wc -c < /tmp/out)
    # The ring holds 10 non-zero bytes and 54 zero bytes; anything non-zero
    # beyond those 10 is somebody else's kernel memory.
    nz=$(tr -d '\000' < /tmp/out | wc -c)
    echo "result[$mod]: exit=$rc bytes_returned=$n nonzero_bytes=$nz $(cat /tmp/err)"
    if [ "$nz" -gt 10 ]; then
        echo "result[$mod]: LEAKED $((nz - 10)) non-zero bytes of other kernel memory; sample:"
        od -A d -t x1 -j 64 -N 48 /tmp/out
    fi

    # Is the device still usable? (A task that died holding the mutex
    # leaves it locked forever, and mutex_lock() can't be interrupted.)
    ( ringctl "$dev" stats > /tmp/st 2>&1 ) &
    pid=$!
    sleep 3
    if kill -0 $pid 2>/dev/null; then
        echo "after[$mod]: device HUNG -- the crashed task never released the ring lock"
    else
        echo "after[$mod]: device usable: $(cat /tmp/st)"
        rmmod "$mod"
    fi
    echo "kernel[$mod]:"
    dmesg | grep -E "BUG|Oops|panicked at|KASAN|Unable to handle|Internal error|WARNING" | head -6
done

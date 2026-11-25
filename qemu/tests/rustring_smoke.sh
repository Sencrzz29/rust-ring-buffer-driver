#!/bin/sh
# Phase 3 smoke test: /dev/rustring basic behaviour and clean error codes.
# (The full functional/adversarial/concurrency suite comes in Phase 5.)
#
# Usage on the board:  sh /tests/rustring_smoke.sh [module] [device]

mod=${1:-rustring}
dev=${2:-/dev/$mod}
EINVAL=22 EBUSY=16 EFAULT=14 ENOTTY=25

pass=0
fail() { echo "FAIL: $*"; rmmod "$mod" 2>/dev/null; exit 1; }
ok()   { pass=$((pass + 1)); echo "ok   $*"; }
# expect_err <errno> <description> <command...>
expect_err() {
    want=$1 what=$2; shift 2
    "$@" >/dev/null 2>&1; got=$?
    [ "$got" -eq "$want" ] || fail "$what: expected errno $want, got $got"
    ok "$what -> errno $want"
}

insmod "/modules/$mod.ko" || fail "insmod"
[ -c "$dev" ] || fail "$dev missing"
ok "insmod creates $dev"

printf 'hello ring' > "$dev" || fail "write"
got=$(cat "$dev")
[ "$got" = "hello ring" ] || fail "read back '$got'"
ok "write then read returns the same bytes"
[ -z "$(cat "$dev")" ] || fail "ring not empty after read"
ok "read on empty ring returns EOF"

printf '0123456789' > "$dev"
[ "$(ringctl "$dev" peek 3 4)" = "3456" ] || fail "peek 3 4"
[ "$(ringctl "$dev" stats | sed 's/.*used \([0-9]*\).*/\1/')" = 10 ] || fail "peek consumed data"
ok "peek returns bytes 3..6 without consuming"

expect_err $EINVAL "peek past end of data"          ringctl "$dev" peek 8 5
expect_err $EINVAL "peek offset 2^64-1 (overflow)"  ringctl "$dev" peek 18446744073709551615 2
expect_err $EINVAL "peek len 2^64-1 (overflow)"     ringctl "$dev" peek 2 18446744073709551615
expect_err $EBUSY  "resize while data present"      ringctl "$dev" resize 128

ringctl "$dev" clear || fail "clear"
ringctl "$dev" resize 64 || fail "resize 64"
[ "$(ringctl "$dev" stats | cut -d' ' -f2)" = 64 ] || fail "capacity not 64"
ok "clear + resize to 64 bytes"
expect_err $EINVAL "resize to 0"                    ringctl "$dev" resize 0
expect_err $EINVAL "resize to 2^60"                 ringctl "$dev" resize 1152921504606846976

dd if=/dev/zero of="$dev" bs=100 count=1 2>/dev/null
[ "$(ringctl "$dev" stats | sed 's/.*used \([0-9]*\).*/\1/')" = 64 ] || fail "partial write did not fill to 64"
ok "100-byte write into 64-byte ring stores 64"
if printf 'x' > "$dev" 2>/dev/null; then fail "write to full ring succeeded"; fi
ok "write to full ring fails (EAGAIN)"

expect_err $EFAULT "GET_STATS into bad pointer"     ringctl "$dev" stats-badptr
expect_err $ENOTTY "unknown ioctl"                  ringctl "$dev" bogus

ringctl "$dev" stats
rmmod "$mod" || fail "rmmod"
[ ! -e "$dev" ] || fail "$dev still present after rmmod"
ok "rmmod removes $dev"

dmesg | grep "$mod:" | tail -3
echo "PASS: $pass checks"

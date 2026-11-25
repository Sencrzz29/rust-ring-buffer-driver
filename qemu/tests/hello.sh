#!/bin/sh
# Phase 1 check: an out-of-tree Rust module loads, shows up, and unloads.
set -e
insmod /modules/rr_hello.ko
grep '^rr_hello ' /proc/modules
rmmod rr_hello
if grep -q '^rr_hello ' /proc/modules; then
    echo "FAIL: rr_hello still loaded"
    exit 1
fi
dmesg | grep 'rr_hello:'
echo "PASS: rr_hello loaded and unloaded"

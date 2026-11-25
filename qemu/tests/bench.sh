#!/bin/sh
# Benchmark rustring and cring side by side.
#   sh /tests/bench.sh [ringbench options]
insmod /modules/rustring.ko
insmod /modules/cring.ko
ringbench "$@" /dev/rustring /dev/cring
rc=$?
rmmod rustring cring
exit $rc

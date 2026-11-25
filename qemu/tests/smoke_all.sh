#!/bin/sh
# Run the smoke test against all four modules in one boot: the two real
# drivers with hostile inputs, the two naive builds with benign inputs only.
rc=0
sh /tests/ring_smoke.sh rustring       full   || rc=1
sh /tests/ring_smoke.sh cring          full   || rc=1
sh /tests/ring_smoke.sh rustring_naive benign || rc=1
sh /tests/ring_smoke.sh cring_naive    benign || rc=1
echo "== kernel warnings during the run: $(dmesg | grep -cE 'BUG:|WARNING:|Oops|possible circular|sleeping function called')"
exit $rc

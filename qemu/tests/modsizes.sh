#!/bin/sh
# In-kernel memory footprint of each module, as reported by /proc/modules.
for mod in rustring cring rustring_naive cring_naive rr_hello; do
    insmod "/modules/$mod.ko" || continue
    awk -v m="$mod" '$1 == m { print "MODSIZE," m "," $2 }' /proc/modules
    rmmod "$mod"
done

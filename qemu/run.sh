#!/usr/bin/env bash
# Boot the emulated ARM64 board.
#
#   qemu/run.sh [options]
#     -k perf|debug     kernel flavour (default: perf)
#     -r <test>         run /tests/<test> unattended, then power off
#     -a "<args>"       extra kernel command-line arguments (e.g. panic_on_oops=1)
#     -l <file>         also write the serial console to <file>
#     -t <seconds>      kill the VM after this long (default 900 with -r, none otherwise)
#     -g                wait for GDB on tcp::1234 (see docs) before booting
#     -n                don't rebuild the initramfs
#
# Board: QEMU "virt" machine, 4x Cortex-A72 (Raspberry Pi 4 class CPU), 1 GiB RAM.
set -euo pipefail
source "$(dirname "$0")/../env/common.sh"

flavour=perf run= append= log= tmo= gdb= rebuild=1
while getopts "k:r:a:l:t:gn" o; do
    case $o in
        k) flavour=$OPTARG ;;  r) run=$OPTARG ;;  a) append=$OPTARG ;;
        l) log=$OPTARG ;;      t) tmo=$OPTARG ;;  g) gdb=1 ;;  n) rebuild= ;;
        *) sed -n '2,15p' "$0"; exit 2 ;;
    esac
done

kdir=$(kbuild_dir "$flavour")
[[ -n $rebuild ]] && "$REPO/qemu/mkinitramfs.sh" "$flavour" >&2

cmdline="console=ttyAMA0 rdinit=/init panic=-1 loglevel=6"
[[ -n $run ]] && cmdline+=" rr.run=$run"
[[ $flavour == debug ]] && cmdline+=" kmemleak=on"
[[ -n $append ]] && cmdline+=" $append"

qemu=(qemu-system-aarch64
      -machine virt -cpu cortex-a72 -smp 4 -m 1024
      -accel tcg,thread=multi
      -nographic -no-reboot -nic none
      -kernel "$kdir/arch/arm64/boot/Image"
      -initrd "$WORK/initramfs/$flavour.cpio.gz"
      -append "$cmdline")
[[ -n $gdb ]] && qemu+=(-s -S)

[[ -z $tmo && -n $run ]] && tmo=900
[[ -n $tmo ]] && qemu=(timeout --foreground -k 5 "$tmo" "${qemu[@]}")

if [[ -n $log ]]; then
    mkdir -p "$(dirname "$log")"
    "${qemu[@]}" 2>&1 | tee "$log"
else
    "${qemu[@]}"
fi

#!/bin/bash
# SD/eMMC boot scenarios on the andr36oid-px30 machine (DW-MMC model + card
# images from mkcard.sh + the PX30 test kernel), with checks.
#
#   sd-scenarios.sh <fw-dir> <runs-dir> [scenario...]
#
# fw-dir holds Mu-r36s.bin and DEBUG_CLANGPDB/ (symbols); PIO=<fw-dir> names a
# second build with PcdDwMmcForcePio=TRUE for the pio* scenarios. Cards are
# built once into $CARDS (default /srv/andr36oid-uefi/emu/cards).
# Scenarios (default all): idmac 512m pio recovery emmc emmc-native native
#   nocard dcrc rto drto dead pio-dcrc
# Each run's files are in <runs-dir>/<scenario>; a summary line per check is
# printed and the script exits 1 if any check failed.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
FW=$(readlink -f "$1"); RUNS=$(readlink -f "$2"); shift 2
PIO=${PIO:-}
CARDS=${CARDS:-${EMU_DIR:-/srv/andr36oid-uefi/emu}/cards}
ALL="idmac 512m pio recovery emmc emmc-native native nocard dcrc rto drto dead pio-dcrc"
LIST=${*:-$ALL}
mkdir -p "$CARDS" "$RUNS"
[ -f "$CARDS/android.img" ] || "$HERE/mkcard.sh" "$CARDS/android.img" 2>/dev/null
[ -f "$CARDS/recovery.img" ] || "$HERE/mkcard.sh" "$CARDS/recovery.img" --recovery 2>/dev/null
fail=0

run() {     # <name> <fw-dir> <scenario> [env...]
    local name=$1 fw=$2 scen=$3; shift 3
    echo "== $name"
    env "$@" SYMBOLS="$fw/DEBUG_CLANGPDB" TIMEOUT=${TIMEOUT:-240} IDLE=${IDLE:-30} \
        "$HERE/run.sh" "$fw/Mu-r36s.bin" "$scen" "$RUNS/$name" > "$RUNS/$name.out" 2>&1
    sed -n '1p' "$RUNS/$name/result.txt"
}
has() {     # <name> <file> <text>
    if grep -aqF -- "$3" "$RUNS/$1/$2"; then echo "ok   $1: $2 has '$3'"; else echo "FAIL $1: $2 lacks '$3'"; fail=1; fi
}
hasnt() {
    if grep -aqF -- "$3" "$RUNS/$1/$2"; then echo "FAIL $1: $2 has '$3'"; fail=1; else echo "ok   $1: $2 has no '$3'"; fi
}
cardok() {  # <name>: uefi-log.txt written and intact, fsck clean
    if grep -q "^FAIL" "$RUNS/$1/card-check.txt" 2>/dev/null || ! grep -q "uefi-log.txt: " "$RUNS/$1/card-check.txt" 2>/dev/null; then
        echo "FAIL $1: card check"; sed 's/^/     /' "$RUNS/$1/card-check.txt" 2>/dev/null | grep -v "BOOT: "; fail=1
    else echo "ok   $1: BOOT/uefi-log.txt written, reads back, fsck clean"; fi
}
booted() {  # <name> <initrd> <boot_devices>
    has "$1" uart.log "TESTKERNEL: RESULT PASS"
    hasnt "$1" uart.log "TESTKERNEL: FAIL"
    has "$1" uart.log "TESTKERNEL: initrd name=$2"
    has "$1" uart.log "androidboot.boot_devices=$3"
    has "$1" uart.log "initrd content intact"
}
want() { [[ " $LIST " == *" $1 "* ]]; }

if want idmac; then
    run idmac "$FW" chainload-1G CARD="$CARDS/android.img"
    booted idmac android ff370000.dwmmc; cardok idmac
    has idmac mmc-sdmmc.txt "by IDMAC"; hasnt idmac mmc-sdmmc.txt "CLOCK:"
fi
if want 512m; then
    run 512m "$FW" chainload-512M CARD="$CARDS/android.img"
    booted 512m android ff370000.dwmmc; cardok 512m
fi
if want pio && [ -n "$PIO" ]; then
    run pio "$PIO" chainload-1G CARD="$CARDS/android.img"
    booted pio android ff370000.dwmmc; cardok pio
    hasnt pio mmc-sdmmc.txt "by IDMAC"; has pio mmc-sdmmc.txt "by FIFO"
fi
if want recovery; then
    run recovery "$FW" chainload-1G CARD="$CARDS/recovery.img"
    booted recovery recovery ff370000.dwmmc; cardok recovery
fi
if want emmc; then
    run emmc "$FW" chainload-1G EMMC="$CARDS/android.img" MKBOOT="--bootpart 0:1 --bootdev emmc"
    booted emmc android ff390000.dwmmc; cardok emmc
fi
if want emmc-native; then
    # the boot ROM started from eMMC, the SD slot is empty (card-detect high)
    run emmc-native "$FW" native-1G EMMC="$CARDS/android.img" MKBOOT="--bootdev emmc"
    booted emmc-native android ff390000.dwmmc; cardok emmc-native
fi
if want native; then
    run native "$FW" native-1G CARD="$CARDS/android.img"
    booted native android ff370000.dwmmc; cardok native
fi
if want nocard; then
    run nocard "$FW" chainload-1G
    has nocard log.txt "boot: menu:"; hasnt nocard uart.log TESTKERNEL
fi
for f in dcrc rto drto dead; do
    want $f || continue
    # the 5th block read from the card goes wrong (dead: and every command after it)
    run $f "$FW" chainload-1G CARD="$CARDS/android.img" MACHINE="sd-fault=$f:5"
    has $f mmc-sdmmc.txt "fault"
    hasnt $f uart.log "TESTKERNEL: FAIL"
    if grep -aqF "TESTKERNEL: RESULT PASS" "$RUNS/$f/uart.log"; then echo "info $f: booted anyway"
    else grep -a "boot: menu" "$RUNS/$f/log.txt" | head -1 | sed "s/^/info $f: /"; fi
done
if want pio-dcrc && [ -n "$PIO" ]; then
    run pio-dcrc "$PIO" chainload-1G CARD="$CARDS/android.img" MACHINE="sd-fault=dcrc:5"
    has pio-dcrc mmc-sdmmc.txt "fault"; hasnt pio-dcrc uart.log "TESTKERNEL: FAIL"
fi
[ $fail = 0 ] && echo "== all checks passed" || { echo "== some checks FAILED"; exit 1; }

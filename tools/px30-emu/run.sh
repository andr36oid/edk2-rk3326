#!/bin/bash
# Boot an andr36oid UEFI image on the andr36oid-px30 QEMU machine.
#
#   run.sh <image> <scenario> <outdir> [extra qemu args...]
#
# scenario: chainload-1G | chainload-512M | native-1G | native-512M
#   chainload = U-Boot's booti hand-over (FDT in x0, U-Boot's screen, ATAGs
#   destroyed); native = miniloader -> uboot.img payload at 0x00200000.
#
# Environment:
#   TIMEOUT=120      seconds of wall time before QEMU is stopped
#   IDLE=25          stop once neither the UART nor the RAM log ring grew for
#                    this long (0 = only TIMEOUT)
#   MKBOOT="..."     extra mkboot.py options (--keep-atags, --no-display, ...)
#   MACHINE="..."    extra machine options, e.g. "gpio-in=1=0xfffffeff,charger=on"
#   SYMBOLS=<dir>    Build/<pkg>/<target>_CLANGPDB for symbolizing (default:
#                    next to the image, DEBUG)
#   QEMU=<path>      default /srv/andr36oid-uefi/emu/build/qemu-system-aarch64
#   CARD=<img>       SD card image (mkcard.sh); copied to <outdir>/card.img and
#                    attached to the SD slot (unset = empty slot, CDETECT high)
#   EMMC=<img>       eMMC image, copied to <outdir>/emmc.img
#   PASS=<text>      stop the run as soon as uart.log contains <text> (default
#                    "TESTKERNEL: power off")
#
# With a card, the DW-MMC command logs land in <outdir>/mmc-sdmmc.txt and
# mmc-emmc.txt, and after the run card-check.txt says whether BOOT/uefi-log.txt
# was written, whether its lines match the firmware's RAM log, and what
# fsck.fat -n thinks of BOOT.
#
# Output in <outdir>: uart.log (UART2), log.txt / log-prev.txt (firmware RAM
# log ring), screen.png, trace.txt (+ trace-summary.txt), regs.bin, ram.bin,
# monitor.txt (registers when stopped), result.txt (how it ended).
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
[ $# -ge 3 ] || { sed -n '2,25p' "$0"; exit 2; }
IMAGE=$(readlink -f "$1"); SCEN=$2; OUT=$3; shift 3
QEMU=${QEMU:-/srv/andr36oid-uefi/emu/build/qemu-system-aarch64}
TIMEOUT=${TIMEOUT:-120}
IDLE=${IDLE:-25}

case "$SCEN" in
    chainload-1G) KIND=chainload; RAM=1024 ;;
    chainload-512M) KIND=chainload; RAM=512 ;;
    native-1G) KIND=native; RAM=1024 ;;
    native-512M) KIND=native; RAM=512 ;;
    *) echo "unknown scenario $SCEN"; exit 2 ;;
esac

mkdir -p "$OUT"
OUT=$(readlink -f "$OUT")
rm -f "$OUT"/{uart.log,trace.txt,regs.bin,monitor.txt,result.txt,qemu.log,monitor.sock}
# shellcheck disable=SC2086
python3 -I "$HERE/mkboot.py" --image "$IMAGE" --scenario $KIND --ram-mb $RAM --out "$OUT" $MKBOOT > "$OUT/mkboot.json"
read -r ENTRY X0 X1 X2 X3 < <(python3 -I -c '
import json,sys; s=json.load(open(sys.argv[1]))
print(hex(s["entry"]), *[hex(x) for x in s["x"]])' "$OUT/scenario.json")

MOPTS="andr36oid-px30,memory-backend=ram,entry=$ENTRY,x0=$X0,x1=$X1,x2=$X2,x3=$X3"
MOPTS="$MOPTS,regs=$OUT/regs.bin,regs-init=$OUT/regs-init.txt,trace=$OUT/trace.txt,mmc-log=$OUT/mmc"
DRIVES=()
rm -f "$OUT/card.img" "$OUT/emmc.img" "$OUT"/mmc-*.txt "$OUT/card-check.txt"
if [ -n "$CARD" ]; then
    cp --sparse=always "$CARD" "$OUT/card.img"
    DRIVES+=(-drive "if=sd,index=0,format=raw,file=$OUT/card.img")
fi
if [ -n "$EMMC" ]; then
    cp --sparse=always "$EMMC" "$OUT/emmc.img"
    DRIVES+=(-drive "if=sd,index=1,format=raw,file=$OUT/emmc.img")
fi
PASS=${PASS:-TESTKERNEL: power off}
[ -n "$MACHINE" ] && MOPTS="$MOPTS,$MACHINE"

START=$(date +%s)
"$QEMU" -machine "$MOPTS" -m ${RAM}M \
    -object memory-backend-file,id=ram,size=${RAM}M,mem-path="$OUT/ram.bin",share=on \
    -display none -serial file:"$OUT/uart.log" \
    -monitor unix:"$OUT/monitor.sock",server=on,wait=off \
    -no-reboot -d guest_errors,unimp -D "$OUT/qemu.log" "${DRIVES[@]}" "$@" &
QPID=$!

mon() { echo "$1" | timeout 5 socat -t 2 - UNIX-CONNECT:"$OUT/monitor.sock" 2>/dev/null | tr -d '\r'; }

how=""
last_size=-1; last_change=$(date +%s)
while kill -0 $QPID 2>/dev/null; do
    now=$(date +%s)
    # progress = UART bytes + bytes written to the firmware's RAM log ring
    # (header at 0x01E00000, u64 Written at +8; the ring is the only log when
    # PcdRkLogUart is off)
    usize=$(stat -c %s "$OUT/uart.log" 2>/dev/null || echo 0)
    lsize=$(od -An -tu8 -j 31457288 -N8 "$OUT/ram.bin" 2>/dev/null | tr -d ' ')
    size=$((usize + ${lsize:-0}))
    if [ "$size" != "$last_size" ]; then last_size=$size; last_change=$now; fi
    if [ $((now - START)) -ge "$TIMEOUT" ]; then how="timeout after ${TIMEOUT}s"; break; fi
    if [ -n "$PASS" ] && grep -aqF -- "$PASS" "$OUT/uart.log" 2>/dev/null; then
        sleep 1; kill -0 $QPID 2>/dev/null || break; how="uart.log has '$PASS'"; break
    fi
    if [ "$IDLE" -gt 0 ] && [ "$size" -gt 0 ] && [ $((now - last_change)) -ge "$IDLE" ]; then
        how="no new log output for ${IDLE}s"; break
    fi
    sleep 1
done
if kill -0 $QPID 2>/dev/null; then
    { echo "== stopped: $how"; mon "info registers"; } > "$OUT/monitor.txt"
    mon "quit" > /dev/null || true
    for _ in 1 2 3 4 5; do kill -0 $QPID 2>/dev/null || break; sleep 1; done
    kill -9 $QPID 2>/dev/null || true
    wait $QPID 2>/dev/null || true
    echo "stopped by run.sh: $how ($(( $(date +%s) - START ))s)" > "$OUT/result.txt"
else
    wait $QPID && rc=0 || rc=$?
    echo "QEMU exited by itself (rc $rc) after $(( $(date +%s) - START ))s: guest reset or power-off (-no-reboot)" > "$OUT/result.txt"
fi

SYM=${SYMBOLS:-$(dirname "$IMAGE")/Build/r36sPkg/DEBUG_CLANGPDB}
python3 -I "$HERE/extract.py" "$OUT" --symbols "$SYM" || true
for disk in card emmc; do
    [ -f "$OUT/$disk.img" ] && python3 -I "$HERE/cardcheck.py" "$OUT/$disk.img" "$OUT" >> "$OUT/card-check.txt" 2>&1 || true
done
cat "$OUT/result.txt"
[ -f "$OUT/card-check.txt" ] && cat "$OUT/card-check.txt"
grep -a "TESTKERNEL: RESULT\|TESTKERNEL: FAIL\|TESTKERNEL: initrd name\|TESTKERNEL: bootargs" "$OUT/uart.log" 2>/dev/null || true

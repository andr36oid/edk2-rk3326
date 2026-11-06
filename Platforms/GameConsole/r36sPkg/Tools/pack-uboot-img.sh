#!/bin/bash
# Pack Mu-r36s.bin as uboot.img, the image the RK3326 miniloader loads from
# sector 16384 to 0x00200000 and BL31 starts. BootShim at the front moves the
# FD to its link address (0x02000000), the same as when U-Boot's booti starts
# it, so one build serves both.
#
#   pack-uboot-img.sh <Mu-r36s.bin> <uboot.img> [loaderimage]
#
# loaderimage comes from Rockchip's U-Boot tools (tools/rockchip/loaderimage.c);
# /andr36oid/uefi/tools/build-loaderimage.sh builds it from the U-Boot source.
set -euo pipefail
IN=$1 OUT=$2 LI=${3:-${LOADERIMAGE:-/andr36oid/uefi/tools/bin/loaderimage}}
# 4 copies of 1 MiB in the 4 MiB slot, each a 2 KiB header plus the payload.
MAX=1046528
SIZE=$(stat -c %s "$IN")
[ "$SIZE" -le "$MAX" ] || { echo "$IN is $SIZE bytes, uboot.img takes at most $MAX" >&2; exit 1; }
[ "$(head -c 64 "$IN" | tail -c 8 | head -c 4)" = "ARMd" ] || { echo "$IN has no BootShim header" >&2; exit 1; }
"$LI" --pack --uboot "$IN" "$OUT" 0x00200000 >/dev/null
# check: unpacking gives back exactly the input
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
"$LI" --unpack --uboot "$OUT" "$T/back.bin" >/dev/null
cmp -s "$IN" "$T/back.bin" || { echo "unpacked payload differs from $IN" >&2; exit 1; }
echo "$OUT: $(stat -c %s "$OUT") bytes, payload $SIZE bytes, load 0x00200000"

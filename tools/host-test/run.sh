#!/bin/bash
# Host tests for the andr36oid boot logic: builds the pure libraries of
# Andr36oidPkg with the host gcc (no UEFI, no cross compiler) and runs them.
#   tools/host-test/run.sh [outdir]
# Needs gcc and dtc/fdtget (device-tree-compiler).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:-$(mktemp -d /tmp/andr36oid-host-test.XXXXXX)}
mkdir -p "$OUT"
PKG=$ROOT/Platforms/GameConsole/Andr36oidPkg
MDE=$ROOT/Mu_Basecore/MdePkg
FDTLIB=$MDE/Library/BaseFdtLib

# MdePkg's FdtLib.c includes <libfdt/libfdt/libfdt.h>; reach it through a
# directory of our own so libfdt's <stdint.h> etc. are the host's, not the
# UEFI stand-ins that sit next to it in BaseFdtLib.
mkdir -p "$OUT/inc"
ln -sfn "$FDTLIB/libfdt" "$OUT/inc/libfdt"

CFLAGS=(-O1 -g -Wall -Wno-unused-function -Wno-incompatible-pointer-types
        -Wno-pointer-sign -Wno-sign-compare -fshort-wchar
        -I "$MDE/Include" -I "$MDE/Include/X64" -I "$PKG/Include" -I "$OUT/inc" -I "$FDTLIB/libfdt/libfdt")
LIBFDT=("$FDTLIB"/libfdt/libfdt/fdt*.c)

echo "== building into $OUT"
# libfdt with the host's libc headers; MdePkg's wrapper with Base.h forced in
# first, the way the UEFI build's AutoGen.h does it.
OBJS=()
for c in "${LIBFDT[@]}"; do
    o=$OUT/$(basename "$c" .c).o
    gcc -O1 -g -c -I "$FDTLIB/libfdt/libfdt" -o "$o" "$c"
    OBJS+=("$o")
done
gcc "${CFLAGS[@]}" -include Base.h -c -o "$OUT/FdtLib.o" "$FDTLIB/FdtLib.c"
OBJS+=("$OUT/FdtLib.o")
gcc "${CFLAGS[@]}" -c -o "$OUT/HostShim.o" "$HERE/HostShim.c"
OBJS+=("$OUT/HostShim.o")
gcc "${CFLAGS[@]}" -o "$OUT/TestLinuxBoot" "$HERE/TestLinuxBoot.c" \
    "$PKG/Library/LinuxBootLib/LinuxImage.c" "$PKG/Library/LinuxBootLib/LinuxFdt.c" "${OBJS[@]}"

dtc -q -I dts -O dtb -o "$OUT/kernel-test.dtb" "$HERE/kernel-test.dts"
dtc -q -I dts -O dtb -o "$OUT/handover-test.dtb" "$HERE/handover-test.dts"
# A real andr36oid Android DTB if you have one (the device repo builds it,
# doesn't carry it): REAL_DTB=/path/rk3326-r36s-android.dtb tools/host-test/run.sh
REAL=${REAL_DTB:-/andr36oid/src/device/gameconsole/r36s/BOOT/rk3326-r36s-android.dtb}
rm -f "$OUT"/fixed-*.dtb

echo "== TestLinuxBoot"
"$OUT/TestLinuxBoot" "$OUT/kernel-test.dtb" "$OUT/handover-test.dtb" "$OUT" $( [ -f "$REAL" ] && echo "$REAL" )

# Read the results back with the system's libfdt (fdtget), not ours.
fail=0
expect() {  # <dtb> <node> <prop> <fdtget flags> <expected>
    local got
    got=$(fdtget $4 "$1" "$2" "$3" 2>&1 || true)
    if [ "$got" = "$5" ]; then
        echo "ok   $(basename "$1") $2 $3"
    else
        echo "FAIL $(basename "$1") $2 $3: got '$got', want '$5'"
        fail=1
    fi
}
D=$OUT/fixed-handover.dtb
expect "$D" /chosen bootargs "-ts" "console=null androidboot.hardware=rk30board"
expect "$D" /chosen linux,initrd-start "-tx" "0 a200000"
expect "$D" /chosen linux,initrd-end "-tx" "0 a323456"
expect "$D" /memory@0 reg "-tx" "0 200000 0 8200000 0 9400000 0 36c00000"
expect "$D" /memory@0 device_type "-ts" "memory"
expect "$D" /reserved-memory/drm-logo@00000000 reg "-tx" "0 3dc00000 0 700000"
expect "$D" /reserved-memory/ramoops@110000 reg "-tx" "0 110000 0 f0000"
expect "$D" /display-subsystem/route/route-dsi logo,width "-tu" "640"
expect "$D" /display-subsystem/route/route-dsi logo,bpp "-tu" "24"
expect "$D" /display-subsystem/route/route-dsi video,vrefresh "-tu" "60"
expect "$D" /display-subsystem/route/route-dsi overscan,left_margin "-tu" "100"
expect "$D" /display-subsystem/route/route-dsi status "-ts" "okay"
if fdtget "$D" /memory@40000000 reg >/dev/null 2>&1; then
    echo "FAIL second memory node still there"; fail=1
else
    echo "ok   second memory node removed"
fi
if fdtget "$D" /display-subsystem/route/route-lvds logo,width >/dev/null 2>&1; then
    echo "FAIL route-lvds got logo properties"; fail=1
else
    echo "ok   route-lvds untouched"
fi
D=$OUT/fixed-fb.dtb
expect "$D" /reserved-memory/drm-logo@00000000 reg "-tx" "0 3f000000 0 12c000"
expect "$D" /chosen bootargs "-ts" "a b c"
if fdtget "$D" /chosen linux,initrd-start >/dev/null 2>&1; then
    echo "FAIL initrd-start without an initrd"; fail=1
else
    echo "ok   no initrd properties without an initrd"
fi
D=$OUT/fixed-moved.dtb
expect "$D" /reserved-memory/drm-logo@00000000 reg "-tx" "0 3e800000 0 12c000"
if fdtget "$D" /display-subsystem/route/route-dsi logo,width >/dev/null 2>&1; then
    echo "FAIL moved screen: U-Boot's route properties copied"; fail=1
else
    echo "ok   moved screen: no route properties from U-Boot"
fi
if [ -f "$REAL" ]; then
    D=$OUT/fixed-real.dtb
    dtc -q -I dtb -O dts -o /dev/null "$D" && echo "ok   real DTB still parses" || { echo "FAIL real DTB"; fail=1; }
    expect "$D" /chosen bootargs "-ts" "fw_devlink=permissive rootwait androidboot.revision=rg351mp"
    expect "$D" /reserved-memory/drm-logo@00000000 reg "-tx" "0 3dc00000 0 700000"
    expect "$D" /display-subsystem/route/route-dsi logo,height "-tu" "480"
    expect "$D" /reserved-memory/ramoops@110000 reg "-tx" "0 110000 0 f0000"
fi

for t in "$HERE"/Test*.extra.sh; do [ -f "$t" ] && . "$t"; done

[ $fail = 0 ] && echo "== host tests passed" || { echo "== host tests FAILED"; exit 1; }

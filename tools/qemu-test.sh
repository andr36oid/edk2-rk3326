#!/bin/bash
# QEMU test of the andr36oid boot app and its Linux handoff.
#   tools/qemu-test.sh [outdir]
#   QEMU_CASES="android menu" tools/qemu-test.sh    (default: all four)
# Needs Mu-qemuvirt.bin (build-mu.sh qemuvirt DEBUG), qemu-system-aarch64,
# aarch64-linux-gnu-gcc, mtools, mkimage (u-boot-tools), dtc, python3.
#
# The firmware runs on QEMU's virt machine (Platforms/QEMU/qemuvirtPkg) with
# the same Andr36oidPkg code as the handhelds. A BOOT FAT image goes into
# guest RAM (MemDiskDxe serves it as the SD card's BOOT partition); the guest
# RAM is a shared file, so what the firmware wrote to BOOT (uefi-log.txt) is
# read back after the run. The "kernel" is tools/qemu-test/testkernel.c: it
# checks the booting.rst handoff and prints what it got on the serial port.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
T=$HERE/qemu-test
OUT=${1:-/srv/andr36oid-uefi/scratch-bootapp/qemu}
FW=$ROOT/Mu-qemuvirt.bin
DISK_ADDR=0x50000000        # PcdMemDiskBase
DISK_MB=32                  # PcdMemDiskSize
RAM_BASE_MB=1024            # guest RAM starts at 1 GB
TIMEOUT=${QEMU_TIMEOUT:-180}
CASES=" ${QEMU_CASES:-android recovery menu poweroff} "
want() { [[ $CASES == *" $1 "* ]]; }
mkdir -p "$OUT"
fail=0

[ -f "$FW" ] || { echo "no $FW: build-mu.sh qemuvirt DEBUG first"; exit 2; }

echo "== test kernel"
# Linked at two addresses: the binaries must match, or some code depends on
# where it was linked (it runs wherever the loader puts it).
for at in 0 0x10000000; do
    aarch64-linux-gnu-gcc -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-pie -no-pie \
        -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -static \
        -Wl,-T,"$T/testkernel.lds" -Wl,--defsym=LINK_BASE=$at -Wl,--build-id=none -Wl,--no-warn-rwx-segments \
        -o "$OUT/testkernel-$at.elf" "$T/testkernel-head.S" "$T/testkernel.c" || exit 2
    aarch64-linux-gnu-objcopy -O binary "$OUT/testkernel-$at.elf" "$OUT/Image-$at" || exit 2
done
cmp -s "$OUT/Image-0" "$OUT/Image-0x10000000" || { echo "FAIL test kernel is not position independent"; exit 2; }
mv "$OUT/Image-0" "$OUT/Image"

# Ramdisks: a payload the test kernel recognises, wrapped like mkimg.sh does.
for rd in android:ramdisk stock:ramdisk-stock recovery:ramdisk-recovery; do
    name=${rd%%:*}; file=${rd#*:}
    { printf 'ANDR36OID-TEST-INITRD:%s\n' "$name"; head -c 300000 /dev/urandom | base64; printf 'INITRD-END\n'; } > "$OUT/$file.cpio"
    mkimage -A arm64 -O linux -T ramdisk -C none -n "andr36oid test $name" -d "$OUT/$file.cpio" "$OUT/$file.uimg" >/dev/null || exit 2
done

# REAL_DTB=<andr36oid's rk3326-r36s-android.dtb>, else a small test DTB
DTB=${REAL_DTB:-/andr36oid/src/device/gameconsole/r36s/BOOT/rk3326-r36s-android.dtb}
[ -f "$DTB" ] || { dtc -q -I dts -O dtb -o "$OUT/test.dtb" "$ROOT/tools/host-test/kernel-test.dts"; DTB=$OUT/test.dtb; }
INI=/andr36oid/src/device/gameconsole/r36s/BOOT/boot.ini-android
[ -f "$INI" ] || INI=$ROOT/tools/host-test/boot.ini-android
HOOK=/andr36oid/work/reports/uefi/testkit/boot.ini-uefi-hook.txt
# R46H device profile (mkimg.sh device_profile) with the UEFI hook appended
sed 's|^setenv android2 "|setenv android2 "androidboot.lcd_density=304 |' "$INI" > "$OUT/boot.ini-profile"
[ -f "$HOOK" ] && cat "$HOOK" >> "$OUT/boot.ini-profile"

# mkboot <image> <src:name>...
mkboot() {
    local img=$1; shift
    rm -f "$img"
    dd if=/dev/zero of="$img" bs=1M count=$DISK_MB status=none
    mformat -i "$img" -v BOOT -T $((DISK_MB * 2048)) -h 64 -s 32 :: || return 1
    for f in "$@"; do
        mcopy -i "$img" "${f%%:*}" "::${f#*:}" || return 1
    done
}

# run <case> <machine> <append> [--when TEXT --send KEYS]...
run() {
    local c=$1 machine=$2 append=$3; shift 3
    local ram=$OUT/$c.ram
    rm -f "$ram" "$OUT/$c.log"
    local args=(-machine "$machine" -cpu cortex-a35 -m 1024
        -object memory-backend-file,id=ram,size=1G,mem-path="$ram",share=on -machine memory-backend=ram
        -kernel "$FW" -device loader,file="$OUT/$c.img",addr=$DISK_ADDR,force-raw=on
        -display none -serial stdio -monitor none -nodefaults)
    [ -n "$append" ] && args+=(-append "$append")
    echo "== $c ($machine)"
    python3 "$T/qemu-run.py" "$OUT/$c.log" "$TIMEOUT" "$@" -- qemu-system-aarch64 "${args[@]}"
    local code=$?
    tr -d '\r' < "$OUT/$c.log" > "$OUT/$c.txt"
    # BOOT as the firmware left it
    dd if="$ram" of="$OUT/$c-after.img" bs=1M skip=$(( (DISK_ADDR >> 20) - RAM_BASE_MB )) count=$DISK_MB status=none
    rm -f "$ram"
    if [ $code = 0 ]; then echo "ok   $c: QEMU powered off"; else echo "FAIL $c: QEMU exit $code (124 = timeout)"; fail=1; fi
}

has() {      # <case> <text>: in the serial output
    if grep -aqF -- "$2" "$OUT/$1.txt"; then echo "ok   $1: $2"; else echo "FAIL $1: no '$2'"; fail=1; fi
}
hasnt() {
    if grep -aqF -- "$2" "$OUT/$1.txt"; then echo "FAIL $1: '$2'"; fail=1; else echo "ok   $1: no '$2'"; fi
}
count() {    # <case> <text> <n>
    local n; n=$(grep -acF -- "$2" "$OUT/$1.txt")
    if [ "$n" = "$3" ]; then echo "ok   $1: $3 x '$2'"; else echo "FAIL $1: $n x '$2', want $3"; fail=1; fi
}
line() {     # <case> <exact line>
    if grep -aqxF -- "$2" "$OUT/$1.txt"; then echo "ok   $1: exact line ${2:0:60}..."; else
        echo "FAIL $1: no line '$2'"; grep -F "TESTKERNEL: bootargs" "$OUT/$1.txt" | sed 's/^/     got: /'; fail=1; fi
}
onboot() {   # <case> <file> <text>: in a file the firmware wrote to BOOT
    if mtype -i "$OUT/$1-after.img" "::$2" 2>/dev/null | tr -d '\r' | grep -qF -- "$3"; then
        echo "ok   $1: BOOT/$2 has '$3'"; else echo "FAIL $1: BOOT/$2 lacks '$3'"; fail=1; fi
}

BASE="fw_devlink=permissive rootwait consoleblank=0 swiotlb=1 coherent_pool=1m firmware_class.path=/vendor/firmware androidboot.hardware=rk30board androidboot.selinux=permissive androidboot.verifiedbootstate=green"
CONSOLE="console=null loglevel=4 printk.time=1 vt.global_cursor_default=0 fiq_debugger.disable=1"
FACTS="andr36oid.hwrev=rg351mp andr36oid.serialno=QEMU0001 andr36oid.ram_mb=1024 andr36oid.bootpart=1:1"
K=$OUT/Image:Image
echo "== DTB: $DTB"
RD="$OUT/ramdisk.uimg:ramdisk.uimg $OUT/ramdisk-stock.uimg:ramdisk-stock.uimg $OUT/ramdisk-recovery.uimg:ramdisk-recovery.uimg"

# 1. EL2, a normal Android start through the chainload hook's facts, with a
#    device profile and Kernel Tweaks; one warm restart for the previous log.
if want android; then
    printf 'kt_args=qemu_test.kt=1 qemu_test.reset_once\n' > "$OUT/kt.txt"
    mkboot "$OUT/android.img" $K $RD "$DTB:rk3326-r36s-android.dtb" "$OUT/boot.ini-profile:boot.ini" "$OUT/kt.txt:kernel-tweaks.txt" || exit 2
    run android "virt,virtualization=on,gic-version=2" "$FACTS"
    has android "TESTKERNEL: started at EL2"
    line android "TESTKERNEL: bootargs=$BASE androidboot.lcd_density=304 androidboot.revision=rg351mp androidboot.serialno=QEMU0001 androidboot.boot_devices=ff370000.dwmmc $CONSOLE qemu_test.kt=1 qemu_test.reset_once"
    has android "TESTKERNEL: initrd name=android"
    has android "TESTKERNEL: memory 0x0000000040000000+0x0000000040000000"
    count android "TESTKERNEL: RESULT PASS" 2
    hasnt android "TESTKERNEL: FAIL"
    has android "TESTKERNEL: warm restart"
    onboot android uefi-log.txt "boot: U-Boot says hwrev=rg351mp ram_mb=1024 bootpart=1:1"
    onboot android uefi-log.txt "boot: starting Linux"
    onboot android uefi-log-prev.txt "boot: starting Linux"
fi

# 2. EL1 (PSCI over HVC), recovery.txt, no facts from U-Boot (no -append):
#    the board's own (QEMU: no buttons, v00, no serial number).
if want recovery; then
    printf 'x\n' > "$OUT/recovery.txt"
    mkboot "$OUT/recovery.img" $K $RD "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" "$OUT/kt.txt:kernel-tweaks.txt" "$OUT/recovery.txt:recovery.txt" || exit 2
    run recovery "virt,gic-version=2" ""
    has recovery "TESTKERNEL: started at EL1"
    line recovery "TESTKERNEL: bootargs=$BASE androidboot.revision=v00 androidboot.serialno= androidboot.boot_devices=ff370000.dwmmc $CONSOLE dw_mmc_rockchip.builtin_emmc=1"
    has recovery "TESTKERNEL: initrd name=recovery"
    count recovery "TESTKERNEL: RESULT PASS" 1
    hasnt recovery "TESTKERNEL: FAIL"
    onboot recovery uefi-log.txt "no facts from U-Boot"
fi

# 3. A failed start (no ramdisk.uimg) opens the menu; Save log, then Recovery.
if want menu; then
    mkboot "$OUT/menu.img" $K "$OUT/ramdisk-recovery.uimg:ramdisk-recovery.uimg" "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" || exit 2
    run menu "virt,virtualization=on,gic-version=2" "$FACTS" --when "andr36oid boot menu" --send 5 --when "Log saved to BOOT" --send 4
    has menu "Could not read BOOT/ramdisk.uimg"
    has menu "Log saved to BOOT/uefi-log.txt"
    has menu "TESTKERNEL: initrd name=recovery"
    count menu "TESTKERNEL: RESULT PASS" 1
    onboot menu uefi-log.txt "Could not read BOOT/ramdisk.uimg"
    onboot menu uefi-log.txt "boot: menu: Recovery"
    onboot menu uefi-log.txt "boot: starting Linux"
fi

# 4. No kernel at all: the menu, Power off from it (ResetSystem over PSCI SMC).
if want poweroff; then
    mkboot "$OUT/poweroff.img" "$INI:boot.ini" || exit 2
    run poweroff "virt,virtualization=on,gic-version=2" "$FACTS" --when "andr36oid boot menu" --send 6
    has poweroff "Could not read BOOT/Image"
    has poweroff "Powering off"
    hasnt poweroff "TESTKERNEL"
    onboot poweroff uefi-log.txt "boot: menu: Power off"
fi

[ $fail = 0 ] && echo "== QEMU tests passed" || { echo "== QEMU tests FAILED (logs in $OUT/*.txt)"; exit 1; }

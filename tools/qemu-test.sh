#!/bin/bash
# QEMU test of the andr36oid boot app and its Linux handoff.
#   tools/qemu-test.sh [outdir]
#   QEMU_CASES="android menu" tools/qemu-test.sh    (default: all)
# Needs Mu-qemuvirt.bin (build-mu.sh qemuvirt DEBUG), qemu-system-aarch64,
# aarch64-linux-gnu-gcc, mtools, mkimage (u-boot-tools), dtc/fdtput, python3.
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
EMMC_ADDR=0x52000000        # the eMMC's BOOT image right after the SD card's
USB_ADDR=0x54000000         # a USB stick's after that
RAM_BASE_MB=1024            # guest RAM starts at 1 GB
TIMEOUT=${QEMU_TIMEOUT:-180}
CASES=" ${QEMU_CASES:-android recovery menu poweroff reset logo emmc emmctest emmcnative usb usbfallback} "
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
for rd in android:ramdisk stock:ramdisk-stock recovery:ramdisk-recovery emmc:ramdisk-emmc usb:ramdisk-usb; do
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
        -display none -serial mon:stdio -nodefaults)
    [ -n "$append" ] && args+=(-append "$append")
    # A bootloader FDT of the case's own (QEMU still adds /chosen and /memory)
    [ -f "$OUT/$c.dtb" ] && args+=(-dtb "$OUT/$c.dtb")
    # An eMMC with its own BOOT
    [ -f "$OUT/$c-emmc.img" ] && args+=(-device loader,file="$OUT/$c-emmc.img",addr=$EMMC_ADDR,force-raw=on)
    # A USB stick
    [ -f "$OUT/$c-usb.img" ] && args+=(-device loader,file="$OUT/$c-usb.img",addr=$USB_ADDR,force-raw=on)
    echo "== $c ($machine)"
    python3 "$T/qemu-run.py" "$OUT/$c.log" "$TIMEOUT" "$@" -- qemu-system-aarch64 "${args[@]}"
    local code=$?
    tr -d '\r' < "$OUT/$c.log" > "$OUT/$c.txt"
    # BOOT as the firmware left it
    dd if="$ram" of="$OUT/$c-after.img" bs=1M skip=$(( (DISK_ADDR >> 20) - RAM_BASE_MB )) count=$DISK_MB status=none
    [ -f "$OUT/$c-emmc.img" ] &&
        dd if="$ram" of="$OUT/$c-emmc-after.img" bs=1M skip=$(( (EMMC_ADDR >> 20) - RAM_BASE_MB )) count=$DISK_MB status=none
    [ -f "$OUT/$c-usb.img" ] &&
        dd if="$ram" of="$OUT/$c-usb-after.img" bs=1M skip=$(( (USB_ADDR >> 20) - RAM_BASE_MB )) count=$DISK_MB status=none
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
noboot() {   # <case> <file>: the firmware did not write it
    if mtype -i "$OUT/$1-after.img" "::$2" >/dev/null 2>&1; then echo "FAIL $1: BOOT/$2 written"; fail=1; else echo "ok   $1: no BOOT/$2"; fi
}
onemmc() {   # <case> <file> <text>: in a file the firmware wrote to the eMMC's BOOT
    if mtype -i "$OUT/$1-emmc-after.img" "::$2" 2>/dev/null | tr -d '\r' | grep -qF -- "$3"; then
        echo "ok   $1: eMMC BOOT/$2 has '$3'"; else echo "FAIL $1: eMMC BOOT/$2 lacks '$3'"; fail=1; fi
}
onusb() {    # <case> <file> <text>: in a file the firmware wrote to the USB stick
    if mtype -i "$OUT/$1-usb-after.img" "::$2" 2>/dev/null | tr -d '\r' | grep -qF -- "$3"; then
        echo "ok   $1: USB stick $2 has '$3'"; else echo "FAIL $1: USB stick $2 lacks '$3'"; fail=1; fi
}
onboot() {   # <case> <file> <text>: in a file the firmware wrote to BOOT
    if mtype -i "$OUT/$1-after.img" "::$2" 2>/dev/null | tr -d '\r' | grep -qF -- "$3"; then
        echo "ok   $1: BOOT/$2 has '$3'"; else echo "FAIL $1: BOOT/$2 lacks '$3'"; fail=1; fi
}

BASE="fw_devlink=permissive rootwait consoleblank=0 swiotlb=1 coherent_pool=1m firmware_class.path=/vendor/firmware androidboot.hardware=rk30board androidboot.selinux=permissive androidboot.verifiedbootstate=green"
CONSOLE="console=null loglevel=4 printk.time=1 vt.global_cursor_default=0 fiq_debugger.disable=1"
FACTS="andr36oid.hwrev=rg351mp andr36oid.serialno=QEMU0001 andr36oid.ram_mb=1024 andr36oid.bootpart=1:1"
K=$OUT/Image:Image
# Without U-Boot's facts: U-Boot's own serial on these consoles (unseeded
# rand(), the same on every console).
SERIAL=c3d9b8674f4b94f6
echo "== DTB: $DTB"
RD="$OUT/ramdisk.uimg:ramdisk.uimg $OUT/ramdisk-stock.uimg:ramdisk-stock.uimg $OUT/ramdisk-recovery.uimg:ramdisk-recovery.uimg"

# 1. EL2, a normal Android start through the chainload hook's facts, with a
#    device profile and Kernel Tweaks; one warm restart for the previous log;
#    an eMMC test (emmc-test.txt) on a console without eMMC.
if want android; then
    printf 'kt_args=qemu_test.kt=1 qemu_test.reset_once\n' > "$OUT/kt.txt"
    printf 'emmc_test=start\n' > "$OUT/emmc-test.txt"
    mkboot "$OUT/android.img" $K $RD "$DTB:rk3326-r36s-android.dtb" "$OUT/boot.ini-profile:boot.ini" "$OUT/kt.txt:kernel-tweaks.txt" \
        "$OUT/emmc-test.txt:emmc-test.txt" "$DTB:rg351mp-uboot.dtb" || exit 2
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
    # Linux reuses the log RAM, so a start that reached it leaves no previous log.
    noboot android uefi-log-prev.txt
    # emmc-test.txt said start, but there is no eMMC: marked tried, SD start
    has android "boot: eMMC test: eMMC not readable here"
    onboot android emmc-test.txt "emmc_test=tried"
    # The screen DTB for hwrev rg351mp goes to the display driver
    has android "boot: screen: BOOT/rg351mp-uboot.dtb ($(stat -L -c %s "$DTB") bytes) for the display: Success"
fi

# 2. EL1 (PSCI over HVC), recovery.txt, no facts from U-Boot (no -append):
#    the board's own (QEMU: no buttons, v00, U-Boot's fixed serial number).
if want recovery; then
    printf 'x\n' > "$OUT/recovery.txt"
    mkboot "$OUT/recovery.img" $K $RD "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" "$OUT/kt.txt:kernel-tweaks.txt" "$OUT/recovery.txt:recovery.txt" \
        "$OUT/recovery.txt:rg351p-uboot.dtb" || exit 2
    run recovery "virt,gic-version=2" ""
    has recovery "TESTKERNEL: started at EL1"
    line recovery "TESTKERNEL: bootargs=$BASE androidboot.revision=v00 androidboot.serialno=$SERIAL androidboot.boot_devices=ff370000.dwmmc $CONSOLE dw_mmc_rockchip.builtin_emmc=1"
    has recovery "TESTKERNEL: initrd name=recovery"
    count recovery "TESTKERNEL: RESULT PASS" 1
    hasnt recovery "TESTKERNEL: FAIL"
    onboot recovery uefi-log.txt "no facts from U-Boot"
    onboot recovery uefi-log.txt "boot: serial number $SERIAL (U-Boot's"
    # hwrev v00: rg351p-uboot.dtb, here not a device tree
    has recovery "boot: screen: BOOT/rg351p-uboot.dtb is not a device tree"
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

# 5. A reset while still in the firmware (menu open, QEMU monitor system_reset):
# the next start keeps that start's log and saves it as uefi-log-prev.txt.
if want reset; then
    mkboot "$OUT/reset.img" $K "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" || exit 2
    run reset "virt,virtualization=on,gic-version=2" "$FACTS" --when "andr36oid boot menu" --send '\x01csystem_reset\n\x01c' \
        --when "andr36oid boot menu" --send 6
    has reset "Could not read BOOT/ramdisk.uimg"
    has reset "Powering off"
    onboot reset uefi-log-prev.txt "Could not read BOOT/ramdisk.uimg"
    onboot reset uefi-log.txt "boot: menu: Power off"
fi

# 6. U-Boot's logo buffer is only reserved in the FDT U-Boot passes (the
# DTB's drm-logo node is a placeholder). A first start shows where kernel,
# initrd and FDT land; then a bootloader FDT reserves 16 MB around that spot
# as the logo buffer. They must move out of it, and the DTB Linux gets must
# reserve it.
if want logo; then
    mkboot "$OUT/logo.img" $K $RD "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" || exit 2
    rm -f "$OUT/logo.dtb"
    run logo "virt,virtualization=on,gic-version=2" "$FACTS" >/dev/null
    at=$(grep -ao "TESTKERNEL: started at EL2, entry 0x[0-9a-f]*" "$OUT/logo.txt" | sed 's/.*entry 0x//')
    [ -n "$at" ] || { echo "FAIL logo: the first start did not reach the kernel"; exit 1; }
    lo=$(printf '%x' $(( (0x$at & ~0x1fffff) - 0x800000 )))
    echo "     logo buffer at 0x$lo+0x1000000 (the first start put the kernel at 0x$at)"
    qemu-system-aarch64 -machine virt,virtualization=on,gic-version=2,dumpdtb="$OUT/logo.dtb" -cpu cortex-a35 -m 1024 \
        -nodefaults -display none >/dev/null 2>&1
    fdtput -c "$OUT/logo.dtb" /reserved-memory &&
        fdtput -t x "$OUT/logo.dtb" /reserved-memory '#address-cells' 2 &&
        fdtput -t x "$OUT/logo.dtb" /reserved-memory '#size-cells' 2 &&
        fdtput "$OUT/logo.dtb" /reserved-memory ranges &&
        fdtput -c "$OUT/logo.dtb" /reserved-memory/drm-logo@$lo &&
        fdtput -t s "$OUT/logo.dtb" /reserved-memory/drm-logo@$lo compatible rockchip,drm-logo &&
        fdtput -t x "$OUT/logo.dtb" /reserved-memory/drm-logo@$lo reg 0 $lo 0 1000000 || exit 2
    run logo "virt,virtualization=on,gic-version=2" "$FACTS"
    has logo "would hit drm-logo@$lo"
    has logo "TESTKERNEL: reserved drm-logo@00000000 0x00000000$lo+0x0000000001000000"
    count logo "TESTKERNEL: RESULT PASS" 1
    hasnt logo "TESTKERNEL: FAIL"
fi

# 7. U-Boot says the system is on the eMMC (andr36oid.bootpart=0:1): BOOT is
# the eMMC's (its own ramdisk, datacard.txt), not the SD card's.
EMMC_FACTS="andr36oid.hwrev=rg351mp andr36oid.serialno=QEMU0001 andr36oid.ram_mb=1024 andr36oid.bootpart=0:1"
EMMC_RD="$OUT/ramdisk-emmc.uimg:ramdisk.uimg"
if want emmc; then
    printf 'datacard=5c0a7e21-0d1b-4e5a-9b8e-2f3c4d5e6f70\n' > "$OUT/datacard.txt"
    mkboot "$OUT/emmc.img" $K $RD "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" || exit 2
    mkboot "$OUT/emmc-emmc.img" $K "$EMMC_RD" "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" "$OUT/datacard.txt:datacard.txt" || exit 2
    run emmc "virt,virtualization=on,gic-version=2" "$EMMC_FACTS"
    line emmc "TESTKERNEL: bootargs=$BASE androidboot.revision=rg351mp androidboot.serialno=QEMU0001 androidboot.boot_devices=ff390000.dwmmc $CONSOLE dw_mmc_rockchip.builtin_emmc=system androidboot.fstab_suffix=emmcsd androidboot.datacard=5c0a7e21-0d1b-4e5a-9b8e-2f3c4d5e6f70"
    has emmc "TESTKERNEL: initrd name=emmc"
    count emmc "TESTKERNEL: RESULT PASS" 1
    onemmc emmc uefi-log.txt "boot: starting Linux"
    noboot emmc uefi-log.txt
fi

# 8. eMMC Installation step 2 from an SD start: emmc-test.txt says start, the
# eMMC is asked for (it is not there before), the test is marked tried on the
# SD card and the eMMC's system starts.
if want emmctest; then
    mkboot "$OUT/emmctest.img" $K $RD "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" "$OUT/emmc-test.txt:emmc-test.txt" || exit 2
    mkboot "$OUT/emmctest-emmc.img" $K "$EMMC_RD" "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" || exit 2
    run emmctest "virt,virtualization=on,gic-version=2" "$FACTS"
    has emmctest "MemDisk: eMMC asked for"
    has emmctest "boot: eMMC test: starting from the eMMC"
    line emmctest "TESTKERNEL: bootargs=$BASE androidboot.revision=rg351mp androidboot.serialno=QEMU0001 androidboot.boot_devices=ff390000.dwmmc $CONSOLE dw_mmc_rockchip.builtin_emmc=system androidboot.fstab_suffix=emmc"
    has emmctest "TESTKERNEL: initrd name=emmc"
    count emmctest "TESTKERNEL: RESULT PASS" 1
    onboot emmctest emmc-test.txt "emmc_test=tried"
    onemmc emmctest uefi-log.txt "boot: eMMC test: starting from the eMMC"
fi

# 9. No U-Boot, no BOOT on the SD card: as U-Boot's board.c does, the eMMC's
# BOOT (asked for first).
if want emmcnative; then
    mkboot "$OUT/emmcnative.img" || exit 2
    mkboot "$OUT/emmcnative-emmc.img" $K "$EMMC_RD" "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" || exit 2
    run emmcnative "virt,gic-version=2" ""
    has emmcnative "boot: no BOOT on the SD card, using the eMMC's"
    line emmcnative "TESTKERNEL: bootargs=$BASE androidboot.revision=v00 androidboot.serialno=$SERIAL androidboot.boot_devices=ff390000.dwmmc $CONSOLE dw_mmc_rockchip.builtin_emmc=system androidboot.fstab_suffix=emmc"
    has emmcnative "TESTKERNEL: initrd name=emmc"
    count emmcnative "TESTKERNEL: RESULT PASS" 1
    onemmc emmcnative uefi-log.txt "boot: starting Linux"
fi

# 10. A failed SD start (no ramdisk.uimg) with a USB stick in: the menu has
# "Start from the USB stick" (5.), which starts the stick's system with the
# same decisions; boot_devices stays the SD card's controller, the log goes
# to the stick.
USB_RD="$OUT/ramdisk-usb.uimg:ramdisk.uimg"
if want usb; then
    mkboot "$OUT/usb.img" $K "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" || exit 2
    mkboot "$OUT/usb-usb.img" $K "$USB_RD" "$DTB:rk3326-r36s-android.dtb" "$OUT/boot.ini-profile:boot.ini" || exit 2
    run usb "virt,virtualization=on,gic-version=2" "$FACTS" --when "5. Start from the USB stick" --send 5
    has usb "Could not read BOOT/ramdisk.uimg"
    has usb "boot: menu: Start from the USB stick"
    has usb "boot: starting from the USB stick"
    line usb "TESTKERNEL: bootargs=$BASE androidboot.lcd_density=304 androidboot.revision=rg351mp androidboot.serialno=QEMU0001 androidboot.boot_devices=ff370000.dwmmc $CONSOLE"
    has usb "TESTKERNEL: initrd name=usb"
    count usb "TESTKERNEL: RESULT PASS" 1
    onusb usb uefi-log.txt "boot: starting Linux"
    onboot usb uefi-log.txt "Could not read BOOT/ramdisk.uimg"
fi

# 11. No BOOT on the SD card (empty) and no eMMC: the USB stick, logged.
if want usbfallback; then
    mkboot "$OUT/usbfallback.img" || exit 2
    mkboot "$OUT/usbfallback-usb.img" $K "$USB_RD" "$DTB:rk3326-r36s-android.dtb" "$INI:boot.ini" || exit 2
    run usbfallback "virt,virtualization=on,gic-version=2" "$FACTS"
    has usbfallback "boot: no BOOT on the SD card or the eMMC, starting from the USB stick"
    line usbfallback "TESTKERNEL: bootargs=$BASE androidboot.revision=rg351mp androidboot.serialno=QEMU0001 androidboot.boot_devices=ff370000.dwmmc $CONSOLE"
    has usbfallback "TESTKERNEL: initrd name=usb"
    count usbfallback "TESTKERNEL: RESULT PASS" 1
    onusb usbfallback uefi-log.txt "starting from the USB stick"
    noboot usbfallback uefi-log.txt
fi

[ $fail = 0 ] && echo "== QEMU tests passed" || { echo "== QEMU tests FAILED (logs in $OUT/*.txt)"; exit 1; }

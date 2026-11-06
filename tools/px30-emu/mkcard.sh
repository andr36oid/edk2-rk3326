#!/bin/bash
# Build an andr36oid SD card (or eMMC) image for the andr36oid-px30 machine,
# with the PX30 test kernel as BOOT/Image.
#
#   mkcard.sh <out.img> [options]
#     --recovery        put recovery.txt on BOOT (the boot app should start
#                       ramdisk-recovery.uimg)
#     --no-ramdisk      leave ramdisk.uimg out
#     --no-kernel       leave Image out
#     --uefi <bin>      put uefi.bin + uefi.txt (uefi=always) on BOOT, like the
#                       chainload test card
#     --inputs <dir>    where boot.ini-android, uboot-dtb and the DTB come from
#                       (default /srv/andr36oid-uefi/emu/inputs/card, copied from
#                       the device repo's BOOT/ so a rebuild there does not
#                       change a run)
#
# Layout as /andr36oid/src/device/gameconsole/r36s/mkimg.sh: GPT, partition 1
# "BOOT" FAT32 (label BOOT) at sectors 32768-557055, system 2, idbloader 3,
# uboot 4, trust 5, vendor 8, metadata 9, misc 10, cache 11. 1 GiB (QEMU's SD
# model wants a power-of-two size), sparse. BOOT holds boot.ini (boot.ini-android
# + the UEFI hook from testkit), rk3326-r36s-android.dtb, the six uboot-dtb names
# mkimg.sh writes (rg351{mp,v,p}-{kernel,uboot}.dtb), logo.bmp, Image (test
# kernel), ramdisk.uimg / ramdisk-stock.uimg / ramdisk-recovery.uimg (mkimage
# -T ramdisk around a payload the test kernel checks: name, FNV-1a, end mark).
#
# Needs aarch64-linux-gnu-gcc, mkimage, parted, mkfs.fat, mtools.
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PATH=$PATH:/usr/sbin:/sbin
IMG=$1; shift || { sed -n '2,25p' "$0"; exit 2; }
IN=${EMU_DIR:-/srv/andr36oid-uefi/emu}/inputs/card
RECOVERY=0; NORD=0; NOKERNEL=0; UEFI=
while [ $# -gt 0 ]; do
    case "$1" in
        --recovery) RECOVERY=1 ;;
        --no-ramdisk) NORD=1 ;;
        --no-kernel) NOKERNEL=1 ;;
        --uefi) UEFI=$2; shift ;;
        --inputs) IN=$2; shift ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done
HOOK=${HOOK:-$(cd "$(dirname "$0")/../.." && pwd)/andr36oid/testkit/boot.ini-uefi-hook.txt}
W=$(mktemp -d ${EMU_DIR:-/srv/andr36oid-uefi/emu}/tmp.mkcard.XXXXXX)
trap 'rm -rf "$W"' EXIT

# Test kernel, linked at 0 (it checks it runs wherever it is put)
aarch64-linux-gnu-gcc -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-pie -no-pie \
    -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -static \
    -Wl,-T,"$HERE/../qemu-test/testkernel.lds" -Wl,--defsym=LINK_BASE=0 -Wl,--build-id=none \
    -Wl,--no-warn-rwx-segments -o "$W/testkernel.elf" "$HERE/../qemu-test/testkernel-head.S" \
    "$HERE/testkernel-px30.c"
aarch64-linux-gnu-objcopy -O binary "$W/testkernel.elf" "$W/Image"

# Ramdisk payloads: 2 MB of seeded data, so a run is repeatable
for rd in android:ramdisk stock:ramdisk-stock recovery:ramdisk-recovery; do
    python3 -I - "${rd%%:*}" "$W/${rd#*:}.cpio" <<'EOF'
import random, sys
name, out = sys.argv[1], sys.argv[2]
r = random.Random(name)
body = ('ANDR36OID-TEST-INITRD:%s\n' % name).encode() + r.randbytes(2 << 20)
h = 0x811c9dc5
for b in body:
    h = ((h ^ b) * 0x01000193) & 0xffffffff
open(out, 'wb').write(body + ('FNV:%08x\n' % h).encode() + b'INITRD-END\n')
EOF
    mkimage -A arm64 -O linux -T ramdisk -C none -n "andr36oid test ${rd%%:*}" \
        -d "$W/${rd#*:}.cpio" "$W/${rd#*:}.uimg" > /dev/null
done

# BOOT contents
mkdir "$W/BOOT"
cp "$IN/boot.ini-android" "$W/BOOT/boot.ini"
[ -f "$HOOK" ] && cat "$HOOK" >> "$W/BOOT/boot.ini"
cp "$IN/rk3326-r36s-android.dtb" "$IN/logo.bmp" "$W/BOOT/"
for name in rg351mp-kernel rg351v-kernel rg351p-kernel rg351mp-uboot rg351v-uboot rg351p-uboot; do
    cp "$IN/uboot-dtb" "$W/BOOT/$name.dtb"
done
[ $NOKERNEL = 1 ] || cp "$W/Image" "$W/BOOT/Image"
cp "$W/ramdisk-stock.uimg" "$W/ramdisk-recovery.uimg" "$W/BOOT/"
[ $NORD = 1 ] || cp "$W/ramdisk.uimg" "$W/BOOT/"
[ $RECOVERY = 1 ] && printf 'recovery\n' > "$W/BOOT/recovery.txt"
if [ -n "$UEFI" ]; then
    cp "$UEFI" "$W/BOOT/uefi.bin"
    printf 'uefi=always\n' > "$W/BOOT/uefi.txt"
fi

# Disk: same partition table as mkimg.sh (AUTOAPK left out)
rm -f "$IMG"
truncate -s 1G "$IMG"
parted -s "$IMG" mktable gpt
parted -s "$IMG" mkpart BOOT fat32 32768s 557055s
parted -s "$IMG" mkpart system ext4 1081344s 2000000s
parted -s "$IMG" mkpart idbloader 64s 16383s
parted -s "$IMG" mkpart uboot 16384s 24575s
parted -s "$IMG" mkpart trust 24576s 32767s
parted -s "$IMG" mkpart placeholder6 2000001s 2000008s
parted -s "$IMG" mkpart placeholder7 2000009s 2000016s
parted -s "$IMG" mkpart vendor ext4 557056s 884735s
parted -s "$IMG" mkpart metadata ext4 884736s 917503s
parted -s "$IMG" mkpart misc 917504s 925695s
parted -s "$IMG" mkpart cache ext4 925696s 1081343s
parted -s "$IMG" rm 7
parted -s "$IMG" rm 6
truncate -s $(( (557055 - 32768 + 1) * 512 )) "$W/boot.img"
mkfs.fat -F 32 -n BOOT "$W/boot.img" > /dev/null
mcopy -s -i "$W/boot.img" "$W/BOOT/"* ::/
dd if="$W/boot.img" of="$IMG" bs=512 seek=32768 conv=notrunc,sparse status=none
echo "$IMG: BOOT = $(mdir -b -i "$W/boot.img" :: | tr '\n' ' ')"

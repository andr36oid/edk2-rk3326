#!/bin/bash
# Build QEMU (aarch64-softmmu only) with the andr36oid-px30 machine.
#
#   qemu-build.sh [emu-dir]
#
# emu-dir (default /srv/andr36oid-uefi/emu) holds dl/, src/qemu-<ver>/ and
# build/. The upstream tarball is fetched once into dl/ (checksum checked),
# unpacked into src/, andr36oid-px30.c and px30-dwmmc.c are copied into src/.../hw/arm/ and
# registered in hw/arm/Kconfig and hw/arm/meson.build, then ninja builds
# build/qemu-system-aarch64. Re-running only recopies the machine file and
# rebuilds.
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
EMU=${1:-/srv/andr36oid-uefi/emu}
VER=11.1.3
SHA256=${QEMU_SHA256:-}
SRC=$EMU/src/qemu-$VER
BUILD=$EMU/build

mkdir -p "$EMU/dl" "$EMU/src"
if [ ! -d "$SRC" ]; then
    tarball=$EMU/dl/qemu-$VER.tar.xz
    [ -f "$tarball" ] || curl -fL -o "$tarball" "https://download.qemu.org/qemu-$VER.tar.xz"
    if [ -n "$SHA256" ]; then
        echo "$SHA256  $tarball" | sha256sum -c -
    fi
    tar -C "$EMU/src" -xf "$tarball"
fi

cp "$HERE/andr36oid-px30.c" "$HERE/px30-dwmmc.c" "$SRC/hw/arm/"
if ! grep -q "ANDR36OID_PX30" "$SRC/hw/arm/Kconfig"; then
    cat >> "$SRC/hw/arm/Kconfig" <<'EOF'

config ANDR36OID_PX30
    bool
    default y
    depends on AARCH64
    depends on TCG
    select ARM_GIC
    select SERIAL_MM
    select USB_DWC2
    select SD
EOF
fi
if ! grep -q "px30-dwmmc.c" "$SRC/hw/arm/meson.build"; then
    sed -i "/andr36oid-px30.c/d" "$SRC/hw/arm/meson.build"
    sed -i "s|^arm_common_ss.add(files('boot.c'))|arm_common_ss.add(when: 'CONFIG_ANDR36OID_PX30', if_true: files('andr36oid-px30.c', 'px30-dwmmc.c'))\n&|" \
        "$SRC/hw/arm/meson.build"
    grep -q "px30-dwmmc.c" "$SRC/hw/arm/meson.build"
fi
if ! grep -q "select SD$" "$SRC/hw/arm/Kconfig"; then
    sed -i "/^config ANDR36OID_PX30/,/^$/s|    select USB_DWC2|&\n    select SD|" "$SRC/hw/arm/Kconfig"
fi

if [ ! -f "$BUILD/build.ninja" ]; then
    mkdir -p "$BUILD"
    (cd "$BUILD" && "$SRC/configure" --target-list=aarch64-softmmu --disable-docs \
        --disable-werror) > "$BUILD/configure.log" 2>&1
fi
ninja -C "$BUILD" -j"$(nproc)" qemu-system-aarch64 > "$BUILD/build.log" 2>&1 || {
    grep -n "error" "$BUILD/build.log" | head -40
    exit 1
}
"$BUILD/qemu-system-aarch64" -machine help | grep andr36oid-px30

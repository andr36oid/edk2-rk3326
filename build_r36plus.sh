#!/bin/bash
# based on the instructions from edk2-platform
set -e
. build_common.sh
# not actually GCC5; it's GCC7 on Ubuntu 18.04.
./build_bootshim.sh
GCC5_AARCH64_PREFIX=aarch64-linux-gnu- build -j$(nproc) -s -n 0 -a AARCH64 -t GCC5 -p RK3326Pkg/Devices/r36plus.dsc
cat BootShim/BootShim.bin workspace/Build/RK3326Pkg/DEBUG_GCC5/FV/RK3326PKG_UEFI.fd > workspace/UEFI
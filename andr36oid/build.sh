#!/bin/bash
# Build the Mu-Silicium UEFI: andr36oid/build.sh [device] [DEBUG|RELEASE]
# Builds the tree in $MU_ROOT (default: the tree this script is in).
# Needs clang/lld 19, the Mu pip requirements (VENV=<venv>) and iasl on
# TOOLS_BIN or PATH. Output: $MU_ROOT/Mu-<device>.bin
set -eo pipefail
cd "${MU_ROOT:-$(dirname "$0")/..}"
[ -n "${VENV:-}" ] && . "$VENV/bin/activate"
export PATH=/usr/lib/llvm-19/bin:${TOOLS_BIN:-}:$PATH
export CLANG_BIN=/usr/lib/llvm-19/bin/
# Mu's build rewrites a module's .depex only while the module still has one:
# one that went away (a library swap) stays on disk and GenFds packs it, since
# the FDF rule takes it "Optional". Clear them and the autogen stamps so every
# module's depex comes from this build (costs ~15 s of autogen).
B="Build/${1:-r36s}Pkg/${2:-DEBUG}_CLANGPDB"
if [ -d "$B" ]; then
    find "$B" \( -path '*/OUTPUT/*.depex' -o -name AutoGenTimeStamp \) -delete
    if [ -d "$B/FV/Ffs" ]; then find "$B/FV/Ffs" -name '*.dpx' -delete; fi
fi
python3 build_uefi.py -d "${1:-r36s}" -r "${2:-DEBUG}"
# r36s: check the BootShim header and payload against the FD (check-image.py)
if [ "${1:-r36s}" = r36s ]; then
    /usr/bin/python3 -I Platforms/GameConsole/r36sPkg/Tools/check-image.py Mu-r36s.bin \
        Build/r36sPkg/${2:-DEBUG}_CLANGPDB/FV/SILICIUM_UEFI.fd 0x02000000 0x000FF000
    /usr/bin/python3 -I Platforms/GameConsole/r36sPkg/Tools/check-depex.py . Build/r36sPkg/${2:-DEBUG}_CLANGPDB
    /usr/bin/python3 -I Platforms/GameConsole/r36sPkg/Tools/emulate-shim.py Mu-r36s.bin 0x02000000 0x000FF000
fi

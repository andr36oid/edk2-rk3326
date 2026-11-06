#!/bin/bash
# Puts a UEFI test on a mounted andr36oid BOOT partition:
#   install-uefi-test.sh <BOOT mount point> <Mu-r36s.bin> [once|always]
# Inserts the uefi.txt hook into boot.ini (once; right after the status LED block),
# copies the firmware as uefi.bin and writes uefi.txt. Removing uefi.txt (or
# uefi=tried after a one-shot) leaves the card starting Android as before.
set -euo pipefail
B=$1 FW=$2 MODE=${3:-once}
HOOK=$(dirname "$0")/boot.ini-uefi-hook.txt
[ -f "$B/boot.ini" ] || { echo "no boot.ini in $B" >&2; exit 1; }
if ! grep -q 'uefi.txt' "$B/boot.ini"; then
    # after the first 'fi' that closes the status LED block (the DTB load)
    awk -v hook="$HOOK" '
        { print }
        !done && /^fi$/ && seen { while ((getline l < hook) > 0) print l; done=1 }
        /andr36oid,u-boot/ { seen=1 }
    ' "$B/boot.ini" > "$B/boot.ini.new"
    grep -q 'uefi.txt' "$B/boot.ini.new" || { echo "hook not inserted" >&2; rm "$B/boot.ini.new"; exit 1; }
    mv "$B/boot.ini.new" "$B/boot.ini"
fi
cp "$FW" "$B/uefi.bin"
printf 'uefi=%s\n' "$MODE" > "$B/uefi.txt"
echo "UEFI test installed ($MODE) on $B"

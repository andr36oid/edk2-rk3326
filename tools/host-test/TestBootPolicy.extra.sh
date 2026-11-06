# Sourced by run.sh: BootPolicyLib against the real boot.ini-android.
echo "== TestBootPolicy"
gcc -O1 -g -Wall -Wno-format -c -o "$OUT/UbootSerialRef.o" "$HERE/UbootSerialRef.c"
gcc "${CFLAGS[@]}" -o "$OUT/TestBootPolicy" "$HERE/TestBootPolicy.c" \
    "$PKG/Library/BootPolicyLib/BootPolicy.c" "$OUT/UbootSerialRef.o"
INI=/andr36oid/src/device/gameconsole/r36s/BOOT/boot.ini-android
[ -f "$INI" ] || INI=$HERE/boot.ini-android
HOOK=/andr36oid/work/reports/uefi/testkit/boot.ini-uefi-hook.txt
[ -f "$HOOK" ] || HOOK=""
"$OUT/TestBootPolicy" "$INI" "$HOOK" || fail=1

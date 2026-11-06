#!/usr/bin/env python3
"""Check the key tables in BootKeysMap.h against the kernel device trees.

Reads the joypad switch nodes of rk3326-r36s-android.dtsi (R36S layout) and
rk3326-r36s-clone-ee.dtsi (EE layout) and compares the pins with what
dump_keys prints. Usage: check-keys-vs-dts.py <dts dir> <dump_keys binary>
"""
import re
import subprocess
import sys

dts_dir, dump = sys.argv[1], sys.argv[2]

SW = {1: "up", 2: "down", 3: "left", 4: "right", 5: "a", 6: "b",
      13: "fn", 15: "l1", 16: "r1", 19: "select", 22: "start"}


def pin(bank, name):
    m = re.match(r"RK_P([ABCD])(\d)", name)
    return int(bank), "ABCD".index(m.group(1)) * 8 + int(m.group(2))


def switches(path):
    text = open(path).read()
    out = {}
    for m in re.finditer(r"\bsw(\d+)\s*\{[^}]*?gpios\s*=\s*<&gpio(\d)\s+(RK_P[ABCD]\d)", text):
        out[int(m.group(1))] = pin(m.group(2), m.group(3))
    return out


base = switches(f"{dts_dir}/rk3326-r36s-android.dtsi")
ee = dict(base)
ee.update(switches(f"{dts_dir}/rk3326-r36s-clone-ee.dtsi"))

want = {}
for fam, table in (("mp", base), ("v", ee)):
    for n, key in SW.items():
        want[(fam, key)] = table[n]

got = {}
for line in subprocess.check_output([dump], text=True).splitlines():
    fam, key, g, p = line.split()
    got[(fam, key)] = (int(g), int(p))

bad = 0
for k in sorted(want):
    if got.get(k) != want[k]:
        print("MISMATCH", k, "dts", want[k], "table", got.get(k))
        bad += 1
for k in sorted(got):
    if k not in want:
        print("EXTRA", k)
        bad += 1
print(f"{len(want)} keys compared, {bad} problems")
sys.exit(1 if bad else 0)

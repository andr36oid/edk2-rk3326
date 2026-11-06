#!/usr/bin/env python3
"""Compare the PLL and CPU rows and the target rates in RkClockMath.h with
U-Boot's clk_px30.c and cru_px30.h. Usage: check-clk-vs-uboot.py <u-boot tree> <dump_clk>"""
import re, subprocess, sys

tree, dump = sys.argv[1], sys.argv[2]
src = open(tree + "/drivers/clk/rockchip/clk_px30.c").read()
hdr = open(tree + "/arch/arm/include/asm/arch-rockchip/cru_px30.h").read()

# PX30_PLL_RATE(rate, refdiv, fbdiv, postdiv1, postdiv2, dsmpd, frac)
pll = {int(m[0]): tuple(int(x) for x in m[1:]) for m in re.findall(
    r"PX30_PLL_RATE\((\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)", src)
    for m in [(m[0], m[1], m[2], m[3], m[4], m[5], m[6])]}
cpu = {int(a): (int(b), int(c)) for a, b, c in re.findall(r"PX30_CPUCLK_RATE\((\d+),\s*(\d+),\s*(\d+)\)", src)}
hz = {}
for name in ("APLL_HZ", "GPLL_HZ", "ACLK_BUS_HZ", "HCLK_BUS_HZ", "PCLK_BUS_HZ", "ACLK_PERI_HZ", "HCLK_PERI_HZ"):
    m = re.search(r"#define\s+%s\s+\((\d+)\s*\*\s*MHz\)" % name, hdr)
    hz[name] = int(m.group(1)) * 1000000

fails = 0
n = 0
for line in subprocess.check_output([dump], text=True).splitlines():
    f = line.split()
    n += 1
    if f[0] == "pll":
        rate, ref, fb, p1, p2 = map(int, f[1:])
        u = pll.get(rate)
        # U-Boot's tuple is (refdiv, fbdiv, postdiv1, postdiv2, dsmpd, frac)
        ok = u is not None and u[:4] == (ref, fb, p1, p2) and u[4] == 1 and u[5] == 0
        print("pll %d: %s (ours %s, U-Boot %s)" % (rate, "ok" if ok else "MISMATCH", (ref, fb, p1, p2), u))
        fails += not ok
    elif f[0] == "cpu":
        rate, a, p = map(int, f[1:])
        ok = cpu.get(rate) == (a, p)
        print("cpu %d: %s (ours %s, U-Boot %s)" % (rate, "ok" if ok else "MISMATCH", (a, p), cpu.get(rate)))
        fails += not ok
    elif f[0] == "hz":
        for k, v in zip(f[1::2], map(int, f[2::2])):
            ok = hz[k] == v
            print("%s: %s (%d vs %d)" % (k, "ok" if ok else "MISMATCH", v, hz[k]))
            fails += not ok
if n == 0:
    fails += 1
sys.exit(1 if fails else 0)

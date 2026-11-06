#!/usr/bin/env python3
"""Compare the USB PHY / clock / reset values in RkUsbPlatformRegs.h (as
printed by dump_usb) with what the kernel source says for PX30: the
rk3328_phy_cfgs table of the Inno USB2 PHY driver (px30-usb2phy falls back to
it), its tuning function, the pm_domains table, the clock tree and the reset ids."""
import re, subprocess, sys

kernel, dump = sys.argv[1], sys.argv[2]
phy = open(kernel + '/drivers/phy/rockchip/phy-rockchip-inno-usb2.c').read()
pm = open(kernel + '/drivers/soc/rockchip/pm_domains.c').read()
clk = open(kernel + '/drivers/clk/rockchip/clk-px30.c').read()
rst = open(kernel + '/include/dt-bindings/clock/px30-cru.h').read()
dts = open(kernel + '/arch/arm64/boot/dts/rockchip/px30.dtsi').read()

out = {}
for line in subprocess.check_output([dump], text=True).splitlines():
    k, *v = line.split()
    out.setdefault(k, []).append(v)

fails = 0
def check(what, got, want):
    global fails
    ok = got == want
    print(('ok   ' if ok else 'FAIL ') + what + ('' if ok else '  got %s want %s' % (got, want)))
    fails += 0 if ok else 1

# The rk3328 table: {offset, bitend, bitstart, disable, enable}
blk = phy[phy.index('rk3328_phy_cfgs[]'):]
blk = blk[:blk.index('{ /* sentinel */ }')]
def reg(name, section=blk):
    m = re.search(r'\.%s\s*=\s*\{\s*(0x[0-9a-fA-F]+),\s*(\d+),\s*(\d+),\s*(0x[0-9a-fA-F]+|\d+),\s*(0x[0-9a-fA-F]+|\d+)\s*\}' % name, section)
    return [int(x, 0) for x in m.groups()]
def prop(r, en):  # property_enable()
    off, hi, lo, dis, ena = r
    mask = ((1 << (hi - lo + 1)) - 1) << lo
    return off, ((mask << 16) | ((ena if en else dis) << lo)) & 0xFFFFFFFF

# the otg port's rows come first in the table, so these are the OTG ones
for name, key, en in (('phy_sus', 'phy_sus', False), ('iddig_output', 'iddig_output', False),
                      ('iddig_en', 'iddig_en', True), ('vbus_det_en', 'vbus_det_en', False),
                      ('clkout_ctl', 'clkout_ctl', True)):
    off, val = prop(reg(name), en)
    got = out[key][0]
    check('%s offset' % key, int(got[0], 0), off)
    check('%s value' % key, int(got[1], 0), val)

# tuning writes, in order
tun = phy[phy.index('static int rk3328_usb2phy_tuning'):]
tun = tun[:tun.index('\n}\n')]
writes = [(int(a, 0), int(b, 0)) for a, b in re.findall(r'regmap_write\(rphy->grf,\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)\)', tun)]
check('tuning writes', [(int(a, 0), int(b, 0)) for a, b in out['tune']], writes)

# power domain bits
m = re.search(r'\[PX30_PD_USB\]\s*=\s*DOMAIN_PX30\(BIT\((\d+)\),\s*BIT\((\d+)\),\s*BIT\((\d+)\)', pm)
pwr, st, req = map(int, m.groups())
check('pd_usb power bit', int(out['pd_usb'][0][1]), pwr)
check('pd_usb status bit', pwr, st)
check('pd_usb idle bit', int(out['pd_usb'][0][3]), req)

# clock gates in CLKGATE_CON(7)
gate = 0
for nm in ('hclk_usb', 'clk_otg_adp', 'hclk_usb_niu', 'hclk_otg'):
    m = re.search(r'"%s".*?PX30_CLKGATE_CON\(7\),\s*(\d+)' % nm, clk, re.S)
    gate |= 1 << int(m.group(1))
check('gate7 bits', int(out['gate7'][0][0], 0), gate)
m = re.search(r'PCLK_USB_GRF.*?PX30_CLKGATE_CON\((\d+)\),\s*(\d+)', clk)
check('pclk_usb_grf gate', (int(m.group(1)), int(m.group(2))), (16, 6))

# resets: id/16 = SOFTRST_CON, id%16 = bit
ids = {n: int(v) for n, v in re.findall(r'#define (SRST_USB2OTG_H|SRST_USB2OTG|SRST_USB2OTG_ADP)\s+(\d+)', rst)}
sr = out['softrst'][0]
for n, bit in (('SRST_USB2OTG_H', sr[1]), ('SRST_USB2OTG', sr[2]), ('SRST_USB2OTG_ADP', sr[3])):
    check(n, (ids[n] // 16, ids[n] % 16), (int(sr[0]), int(bit)))

# the controller and the PHY node addresses
check('otg base', 'reg = <0x0 0xff300000 0x0 0x40000>' in dts, True)
check('usb2phy grf base', 'reg = <0x0 0xff2c0000 0x0 0x10000>' in dts and 'reg = <0x100 0x10>' in dts, True)

print('%d failed' % fails)
sys.exit(1 if fails else 0)

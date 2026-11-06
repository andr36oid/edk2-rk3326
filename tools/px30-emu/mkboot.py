#!/usr/bin/env python3
"""Lay out RAM and registers for an andr36oid-px30 boot scenario.

  mkboot.py --image Mu-r36s.bin --scenario chainload|native --ram-mb 1024|512 --out DIR
            [--dtb DEVICE.dtb] [--uboot-dtb uboot-dtb] [--logo logo.bmp]
            [--keep-atags] [--no-display] [--bootdev sd|emmc] [--bootpart D:P] [--tee-size MB] [--x0 HEX] [--bootargs-extra STR]

Writes DIR/ram.bin (the guest RAM, used as QEMU's memory-backend-file),
DIR/regs-init.txt (register state at the hand-over), DIR/fdt.dtb (the FDT
passed in x0, chainload only), DIR/atags.bin and DIR/scenario.json (entry, x0-x3,
RAM size, framebuffer).

The state is what the boot chain leaves behind, rebuilt from its sources:

chainload: U-Boot (odroidgoa fork, /andr36oid/work/u-boot) ran boot.ini with the
  UEFI hook (testkit/boot.ini-uefi-hook.txt) and did
  "booti 0x01ffff00 - 0x01f00000" with fdt_high=~0:
  - image at 0x01ffff00, PC there, x0 = 0x01f00000, x1-x3 = 0, EL2, MMU and
    caches off (arch/arm/lib/bootm.c boot_jump_linux, cpu.c cleanup_before_linux)
  - FDT = the device DTB (BOOT/rk3326-r36s-android.dtb) loaded at 0x01f00000
    and fixed up in place by image_setup_libfdt(): arch_fixup_fdt ->
    rockchip_display_fixup (GOADV: drm-logo reserved-memory, logo,* and video,*
    on the route node) and the /memory banks (bidram: DRAM minus ATF 0-1 MB,
    SHM 1-2 MB and OP-TEE; CONFIG_NR_DRAM_BANKS = 12 entries); fdt_root
    (serial-number); fdt_chosen (bootargs from the hook, merged with the DTB's);
    fdt_shrink_to_minimum (memreserve for the FDT, size rounded to a page)
  - miniloader ATAGs at 0x001fe000 written, then the first sizeof(struct tag)
    = 812 bytes zeroed by atags_destroy() in board_quiesce_devices()
  - PMUGRF OS_REG2/3 = DDR geometry (sdram_share.h encoders)
  - clocks as U-Boot's clk_px30.c leaves them (APLL 600, GPLL 1200, NPLL 1188,
    bus/peri dividers from the pmucru assigned-clocks)
  - screen: lcd_init() (rockchip_display_cmds.c): VOPB WIN1 RGB888 (rb_swap
    set for GOADV, flipped back by set_plane on VOP 2.6, so clear),
    640x480 from U-Boot's panel timing (uboot-dtb), framebuffer at
    get_drm_memory() (video_reserve: below the page tables at the top of RAM,
    1 MB aligned, 3 MB FB + 32 MB pool), cleared, logo.bmp drawn by
    bmpdecoder (rows copied as they are in the BMP, bottom-up flipped)
native: the miniloader loaded the payload (Mu-r36s.bin in uboot.img) to
  0x00200000 and jumped there at EL2: no FDT (x0 junk), ATAGs intact, no
  screen, OS_REG2/3 set; clocks: an assumed miniloader state (APLL 816, GPLL
  1188), the miniloader's real values are not known.
"""
import argparse
import json
import os
import struct
import sys

# --------------------------------------------------------------------------
# Minimal FDT reader/writer

FDT_MAGIC = 0xD00DFEED
BEGIN_NODE, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9


class Node:
    def __init__(self, name):
        self.name = name
        self.props = []      # [name, bytes]
        self.children = []

    def prop(self, name):
        for p in self.props:
            if p[0] == name:
                return p[1]
        return None

    def set(self, name, value):
        for p in self.props:
            if p[0] == name:
                p[1] = value
                return
        self.props.append([name, value])

    def child(self, name):
        for c in self.children:
            if c.name == name or (('@' not in name) and c.name.split('@')[0] == name):
                return c
        return None


def fdt_parse(blob):
    (magic, totalsize, off_struct, off_strings, off_rsv, version, _lcv, cpuid,
     size_strings, size_struct) = struct.unpack_from('>10I', blob, 0)
    if magic != FDT_MAGIC:
        raise SystemExit('not an FDT')
    rsv = []
    pos = off_rsv
    while True:
        a, s = struct.unpack_from('>QQ', blob, pos)
        pos += 16
        if a == 0 and s == 0:
            break
        rsv.append((a, s))
    strings = blob[off_strings:off_strings + size_strings]
    pos = off_struct
    stack = []
    root = None
    while True:
        tok, = struct.unpack_from('>I', blob, pos)
        pos += 4
        if tok == BEGIN_NODE:
            end = blob.index(b'\0', pos)
            node = Node(blob[pos:end].decode())
            pos = (end + 4) & ~3
            if stack:
                stack[-1].children.append(node)
            else:
                root = node
            stack.append(node)
        elif tok == END_NODE:
            stack.pop()
        elif tok == PROP:
            ln, nameoff = struct.unpack_from('>II', blob, pos)
            pos += 8
            name = strings[nameoff:strings.index(b'\0', nameoff)].decode()
            stack[-1].props.append([name, bytes(blob[pos:pos + ln])])
            pos = (pos + ln + 3) & ~3
        elif tok == NOP:
            pass
        elif tok == END:
            break
        else:
            raise SystemExit('bad FDT token %d' % tok)
    return root, rsv, cpuid


def fdt_serialize(root, rsv, cpuid=0, totalsize=None):
    strtab = bytearray()
    offs = {}
    st = bytearray()

    def stroff(name):
        if name not in offs:
            offs[name] = len(strtab)
            strtab.extend(name.encode() + b'\0')
        return offs[name]

    def emit(node):
        st.extend(struct.pack('>I', BEGIN_NODE))
        nm = node.name.encode() + b'\0'
        st.extend(nm + b'\0' * (-len(nm) % 4))
        for name, val in node.props:
            st.extend(struct.pack('>III', PROP, len(val), stroff(name)))
            st.extend(val + b'\0' * (-len(val) % 4))
        for c in node.children:
            emit(c)
        st.extend(struct.pack('>I', END_NODE))

    emit(root)
    st.extend(struct.pack('>I', END))
    off_rsv = 40
    rsvb = b''.join(struct.pack('>QQ', a, s) for a, s in rsv) + struct.pack('>QQ', 0, 0)
    off_struct = off_rsv + len(rsvb)
    off_strings = off_struct + len(st)
    end = off_strings + len(strtab)
    total = totalsize if totalsize is not None else end
    if total < end:
        raise SystemExit('FDT does not fit')
    hdr = struct.pack('>10I', FDT_MAGIC, total, off_struct, off_strings, off_rsv, 17, 16,
                      cpuid, len(strtab), len(st))
    out = hdr + rsvb + bytes(st) + bytes(strtab)
    return out + b'\0' * (total - len(out)), off_strings + len(strtab)


def node_at(root, path):
    n = root
    for part in [p for p in path.split('/') if p]:
        n = n.child(part)
        if n is None:
            return None
    return n


def find_compatible(node, compat, path=''):
    me = path + '/' + node.name if path or node.name else ''
    c = node.prop('compatible')
    if c and compat.encode() in c.split(b'\0'):
        return node
    for ch in node.children:
        r = find_compatible(ch, compat, me)
        if r is not None:
            return r
    return None


def u32(v):
    return struct.pack('>I', v)


def cells(root):
    ac = struct.unpack('>I', root.prop('#address-cells'))[0]
    sc = struct.unpack('>I', root.prop('#size-cells'))[0]
    return ac, sc


def pack_reg(root, pairs):
    ac, sc = cells(root)
    out = b''
    for a, s in pairs:
        out += struct.pack('>Q', a) if ac == 2 else struct.pack('>I', a)
        out += struct.pack('>Q', s) if sc == 2 else struct.pack('>I', s)
    return out

# --------------------------------------------------------------------------
# Miniloader ATAGs (arch/arm/include/asm/arch-rockchip/rk_atags.h)

ATAG_CORE, ATAG_SERIAL, ATAG_BOOTDEV, ATAG_DDR_MEM, ATAG_TOS_MEM = (
    0x54410001, 0x54410050, 0x54410051, 0x54410052, 0x54410053)
BOOT_TYPE_EMMC, BOOT_TYPE_SD0 = 1 << 1, 1 << 2


def js_hash(data):
    h = 0x47C6A7E6
    for b in data:          # char is unsigned on ARM/AArch64
        h ^= ((h << 5) + b + (h >> 2)) & 0xFFFFFFFF
        h &= 0xFFFFFFFF
    return h


def tag(magic, payload, hashed=True):
    if not hashed:
        body = struct.pack('<II', (8 + len(payload)) // 4, magic) + payload
        return body
    size = (8 + len(payload) + 4) // 4
    body = struct.pack('<II', size, magic) + payload
    return body + struct.pack('<I', js_hash(body))


def build_atags(ram_bytes, tee_base, tee_size, bootdev=BOOT_TYPE_SD0):
    t = b''
    t += tag(ATAG_CORE, struct.pack('<III', 0, 0, 0), hashed=False)
    # tag_serial: version, enable, addr, baudrate, m_mode, id, reserved[2]
    t += tag(ATAG_SERIAL, struct.pack('<IIQIII8x', 0, 1, 0xFF160000, 1500000, 0, 2))
    # tag_bootdev: version, devtype, devnum, mode, sdupdate, reserved[6]
    t += tag(ATAG_BOOTDEV, struct.pack('<IIIII24x', 0, bootdev, 0, 0, 0))
    # tag_ddr_mem: count, version, bank[20] (bases then sizes), reserved[3]
    banks = [0] * 20
    banks[0], banks[1] = 0, ram_bytes
    t += tag(ATAG_DDR_MEM, struct.pack('<II', 1, 0) + struct.pack('<20Q', *banks) + b'\0' * 12)
    # tag_tos_mem: version, tee_mem {name[8], phy_addr, size, flags}, drm_mem {...},
    # reserved u64[7], reserved1
    tee = struct.pack('<8sQII', b'op-tee', tee_base, tee_size, 1)
    drm = struct.pack('<8sQII', b'', 0, 0, 0)
    t += tag(ATAG_TOS_MEM, struct.pack('<I', 0) + tee + drm + b'\0' * 56 + b'\0' * 4)
    t += struct.pack('<II', 0, 0)       # ATAG_NONE
    return t

# --------------------------------------------------------------------------
# DDR geometry in PMUGRF OS_REG2/3 (U-Boot sdram_share.h SYS_REG_ENC_*, as
# sdram_org_config() writes them; decoded by rockchip_sdram_size()): one
# channel of DDR3, one rank, 10 columns, 8 banks, 15 rows, x16 dies, a 32-bit
# bus (1 GB) or a 16-bit bus (512 MB), record version 2.

def os_regs(ram_mb):
    bw = {1024: 2, 512: 1}[ram_mb]          # log2(bus width in bytes)
    dbw = 1                                  # x16 dies
    reg2 = reg3 = 0
    reg2 |= 3 << 13                          # SYS_REG_ENC_DDRTYPE(DDR3)
    reg2 |= (1 - 1) << 12                    # SYS_REG_ENC_NUM_CH(1)
    reg2 |= 1 << 28                          # SYS_REG_ENC_CHINFO(0)
    reg2 |= (1 - 1) << 11                    # SYS_REG_ENC_RANK(1, 0)
    reg2 |= (10 - 9) << 9                    # SYS_REG_ENC_COL(10, 0)
    reg2 |= 0 << 8                           # SYS_REG_ENC_BK(3, 0)
    reg2 |= (2 >> bw) << 2                   # SYS_REG_ENC_BW
    reg2 |= (2 >> dbw) << 0                  # SYS_REG_ENC_DBW
    reg2 |= ((15 - 13) & 3) << 6             # SYS_REG_ENC_CS0_ROW(15)
    reg3 |= (((15 - 13) & 4) >> 2) << 5
    reg3 |= (10 - 9) << 0                    # SYS_REG_ENC_CS1_COL(10, 0)
    reg3 |= 2 << 28                          # SYS_REG_ENC_VERSION(2)
    return reg2, reg3

# --------------------------------------------------------------------------


def panel_mode(uboot_dtb):
    root, _, _ = fdt_parse(open(uboot_dtb, 'rb').read())
    found = []

    def walk(n):
        if n.name == 'display-timings':
            native = n.prop('native-mode')
            for c in n.children:
                ph = c.prop('phandle')
                if native is None or ph == native:
                    found.append(c)
        for c in n.children:
            walk(c)
    walk(root)
    if not found:
        raise SystemExit('no display-timings in %s' % uboot_dtb)
    t = found[0]
    g = lambda k: struct.unpack('>I', t.prop(k))[0]
    return {k: g(k) for k in ('clock-frequency', 'hactive', 'vactive', 'hfront-porch',
                              'hsync-len', 'hback-porch', 'vfront-porch', 'vsync-len',
                              'vback-porch')}


def route_path(uboot_dtb):
    """The route node rockchip_display.c uses (status okay under /display-subsystem/route)."""
    root, _, _ = fdt_parse(open(uboot_dtb, 'rb').read())
    route = node_at(root, '/display-subsystem/route')
    for c in route.children:
        st = c.prop('status')
        if st is None or st.rstrip(b'\0') in (b'okay', b'ok'):
            return '/display-subsystem/route/' + c.name
    raise SystemExit('no enabled route')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--image', required=True)
    ap.add_argument('--scenario', choices=['chainload', 'native'], required=True)
    ap.add_argument('--ram-mb', type=int, choices=[512, 1024], default=1024)
    ap.add_argument('--out', required=True)
    inputs = '/srv/andr36oid-uefi/emu/inputs'
    ap.add_argument('--dtb', default=os.path.join(inputs, 'rk3326-r36s-android.dtb'))
    ap.add_argument('--uboot-dtb', default=os.path.join(inputs, 'rg351mp-uboot.dtb'))
    ap.add_argument('--logo', default=os.path.join(inputs, 'logo.bmp'))
    ap.add_argument('--keep-atags', action='store_true',
                    help='chainload: leave the ATAGs intact (as if atags_destroy did not run)')
    ap.add_argument('--no-display', action='store_true', help='chainload: no screen from U-Boot')
    ap.add_argument('--tee-size', type=int, default=16, help='OP-TEE size in MB (ATAG and U-Boot bidram)')
    ap.add_argument('--x0', default='0x00300000', help='native: junk left in x0')
    ap.add_argument('--hwrev', default='rg351mp')
    ap.add_argument('--serial', default='c4e8f0d2a17b3956')
    ap.add_argument('--bootpart', default='1:1')
    ap.add_argument('--bootdev', choices=['sd', 'emmc'], default='sd',
                    help='boot device in the miniloader ATAG (what the boot ROM started from)')
    ap.add_argument('--bootargs-extra', default='')
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    ram_bytes = a.ram_mb << 20
    image = open(a.image, 'rb').read()
    segs = []          # (addr, bytes)
    regs = {}          # addr -> value
    tee_base, tee_size = 0x08400000, a.tee_size << 20

    # DDR geometry
    regs[0xFF010208], regs[0xFF01020C] = os_regs(a.ram_mb)

    # ATAGs
    atags = build_atags(ram_bytes, tee_base, tee_size,
                        BOOT_TYPE_EMMC if a.bootdev == 'emmc' else BOOT_TYPE_SD0)
    open(os.path.join(a.out, 'atags.bin'), 'wb').write(atags)
    if a.scenario == 'chainload' and not a.keep_atags:
        atags = b'\0' * 812 + atags[812:]      # atags_destroy(): memset(sizeof(struct tag))
    segs.append((0x001FE000, atags))

    info = {'scenario': a.scenario, 'ram_mb': a.ram_mb, 'x': [0, 0, 0, 0]}

    def pll(base, refdiv, fbdiv, pd1, pd2):
        regs[base + 0] = (pd1 << 12) | fbdiv
        regs[base + 4] = (1 << 12) | (pd2 << 6) | refdiv      # DSMPD, integer mode, powered up

    if a.scenario == 'chainload':
        # clk_px30.c: APLL_HZ 600, GPLL_HZ 1200, NPLL_HZ 1188 (px30_pll_rates);
        # CPLL/DPLL: not set by U-Boot (CPLL for dclk is outside its rate table),
        # assumed 1200 / 666.
        pll(0xFF2B0000, 1, 75, 3, 1)       # APLL 600
        pll(0xFF2B0020, 1, 111, 4, 1)      # DPLL 666 (DDR, assumed)
        pll(0xFF2B0040, 1, 50, 1, 1)       # CPLL 1200 (assumed)
        pll(0xFF2B0060, 2, 99, 1, 1)       # NPLL 1188
        pll(0xFF2BC000, 1, 50, 1, 1)       # GPLL 1200 (PMUCRU)
        regs[0xFF2B0100] = (1 << 12) | (3 << 8)      # CLKSEL0: ARMCLK 600 aclk_div 1, pclk_div 3
        regs[0xFF2B0138] = (7 << 8) | 5               # CLKSEL14: peri aclk 200, hclk 150
        regs[0xFF2B015C] = (5 << 8)                   # CLKSEL23: bus aclk 200 from GPLL
        regs[0xFF2B0160] = (1 << 8) | 7               # CLKSEL24: bus pclk 100, hclk 150
    else:
        # assumed miniloader state (same start point as the board host test)
        pll(0xFF2B0000, 1, 68, 2, 1)       # APLL 816
        pll(0xFF2B0020, 1, 111, 4, 1)      # DPLL 666
        pll(0xFF2B0040, 1, 50, 1, 1)       # CPLL 1200
        pll(0xFF2B0060, 2, 99, 1, 1)       # NPLL 1188
        pll(0xFF2BC000, 2, 99, 1, 1)       # GPLL 1188
        regs[0xFF2B0100] = (1 << 12) | (3 << 8)
        regs[0xFF2B0138] = (7 << 8) | 5
        regs[0xFF2B015C] = (5 << 8)
        regs[0xFF2B0160] = (1 << 8) | 7
    regs[0xFF2B00A0] = 0x55                # MODE_CON: APLL, CPLL, DPLL, NPLL normal
    regs[0xFF2BC020] = 0x1                 # PMU_MODE: GPLL normal

    fb = None
    if a.scenario == 'chainload':
        entry = 0x01FFFF00
        info['x'] = [0x01F00000, 0, 0, 0]
        segs.append((entry, image))
        if len(image) > 0x02000000 + 0x000FF000 - entry + 0x100:
            raise SystemExit('image too big')

        # framebuffer (video_reserve / alloc_fb / get_drm_memory)
        tlb = 0x8000                                     # get_page_table_size(): 2 tables x2 + 4
        relocaddr = (ram_bytes - tlb) & ~0xFFFF          # reserve_round_4k + reserve_mmu
        fb_size = 1024 * 768 * 4                         # DRM_ROCKCHIP_FB_SIZE (GOADV)
        drm_mem = (relocaddr - (fb_size + 32 * 1024 * 1024)) & ~0xFFFFF
        mode = panel_mode(a.uboot_dtb)
        w, h = mode['hactive'], mode['vactive']
        bmp = open(a.logo, 'rb').read()
        rb = route_path(a.uboot_dtb)
        if not a.no_display:
            if bmp[:2] != b'BM' or struct.unpack_from('<H', bmp, 28)[0] != 24:
                raise SystemExit('logo must be a 24-bit BMP')
            bw_, bh_ = struct.unpack_from('<ii', bmp, 18)
            dataoff = struct.unpack_from('<I', bmp, 10)[0]
            stride = (bw_ * 3 + 3) & ~3
            fbmem = bytearray(w * h * 3)                  # lcd_init memset
            flip = bh_ > 0
            bh_ = abs(bh_)
            dst_stride = (bw_ * 3 + 3) & ~3              # bmpdecoder: dst stride = src stride
            for row in range(bh_):
                src_row = (bh_ - 1 - row) if flip else row
                line = bmp[dataoff + src_row * stride: dataoff + src_row * stride + 3 * bw_]
                off = row * dst_stride
                fbmem[off:off + len(line)] = line
            segs.append((drm_mem, bytes(fbmem)))
            segs.append((drm_mem + fb_size, bmp))         # bmp_mem: logo.bmp as loaded
            fb = {'base': drm_mem, 'width': w, 'height': h, 'stride': w * 3, 'bpp': 24}
            # VOPB (rockchip_vop.c, RK3366_LIT offsets in rockchip_vop_reg.h)
            vop = 0xFF460000
            htotal = w + mode['hfront-porch'] + mode['hsync-len'] + mode['hback-porch']
            hss = w + mode['hfront-porch']
            vtotal = h + mode['vfront-porch'] + mode['vsync-len'] + mode['vback-porch']
            vss = h + mode['vfront-porch']
            hact_st, vact_st = htotal - hss, vtotal - vss
            regs[vop + 0x18] = 0                              # SYS_CTRL2: standby off
            # WIN1: enable, RGB888, rb_swap clear: lcd_init() asks for rb_swap
            # (CONFIG_PLATFORM_ODROID_GOADV), then rockchip_vop.c set_plane
            # flips it back for RGB888 on VOP 2.6 ("PX30 treat rgb888 as bgr888")
            regs[vop + 0x90] = 1 | (1 << 4)
            regs[vop + 0x98] = ((w * 24 + 31) // 32)          # xvir in words
            regs[vop + 0xA0] = drm_mem                        # yrgb_mst
            regs[vop + 0xA4] = ((h - 1) << 16) | (w - 1)      # dsp_info
            regs[vop + 0xA8] = (vact_st << 16) | hact_st      # dsp_st
            regs[vop + 0x100] = (htotal << 16) | mode['hsync-len']
            regs[vop + 0x104] = (hact_st << 16) | (hact_st + w)
            regs[vop + 0x108] = (vtotal << 16) | mode['vsync-len']
            regs[vop + 0x10C] = (vact_st << 16) | (vact_st + h)
        info['fb'] = fb

        # FDT fixups (image_setup_libfdt order)
        root, rsv, cpuid = fdt_parse(open(a.dtb, 'rb').read())
        if not a.no_display:
            logo = find_compatible(root, 'rockchip,drm-logo')
            if logo is not None:
                logo.set('reg', pack_reg(root, [(drm_mem, fb_size + 4 * 1024 * 1024)]))
                rn = node_at(root, rb)
                if rn is None:
                    print('warning: route %s not in the DTB (U-Boot prints "Unable to update")' % rb)
                else:
                    vals = [('logo,offset', 0), ('logo,width', w), ('logo,height', h),
                            ('logo,bpp', 24), ('logo,ymirror', 0),
                            ('video,hdisplay', w), ('video,vdisplay', h),
                            ('video,crtc_hsync_end', hss + mode['hsync-len']),
                            ('video,crtc_vsync_end', vss + mode['vsync-len']),
                            ('video,vrefresh', round(mode['clock-frequency'] / (htotal * vtotal))),
                            ('video,flags', 0xA),   # NHSYNC|NVSYNC (hsync/vsync-active 0)
                            ('overscan,left_margin', 100), ('overscan,right_margin', 100),
                            ('overscan,top_margin', 100), ('overscan,bottom_margin', 100)]
                    for k, v in vals:
                        rn.set(k, u32(v))
        # /memory: bi_dram banks after bidram reserves ATF 0-1M, SHM 1-2M, OP-TEE
        banks = [(0x00200000, tee_base - 0x00200000), (tee_base + tee_size, ram_bytes - tee_base - tee_size)]
        banks += [(0, 0)] * (12 - len(banks))
        mem = root.child('memory')
        if mem is None:
            mem = Node('memory')
            root.children.insert(0, mem)
        mem.set('device_type', b'memory\0')
        mem.set('reg', pack_reg(root, banks))
        ram_mb_env = sum(s for _, s in banks) >> 20
        root.set('serial-number', a.serial.encode() + b'\0')
        bootargs = 'andr36oid.hwrev=%s andr36oid.serialno=%s andr36oid.ram_mb=%d andr36oid.bootpart=%s' % (
            a.hwrev, a.serial, ram_mb_env, a.bootpart)
        if a.bootargs_extra:
            bootargs += ' ' + a.bootargs_extra
        chosen = root.child('chosen')
        if chosen is None:
            chosen = Node('chosen')
            root.children.insert(0, chosen)
        dtb_args = (chosen.prop('bootargs') or b'').rstrip(b'\0').decode()
        merged = bootargs.split(' ')
        for item in dtb_args.split():          # env_update(): replace same title, else append
            if '=' in item:
                title = item.split('=')[0]
                for i, ai in enumerate(merged):
                    if '=' in ai and ai.split('=')[0] == title:
                        merged[i] = item
                        break
                else:
                    merged.append(item)
            else:
                merged.append(item)
        chosen.set('bootargs', (' '.join(merged)).encode() + b'\0')
        info['bootargs'] = ' '.join(merged)
        # fdt_shrink_to_minimum(blob, 0) with the blob at 0x01f00000
        blob_addr = 0x01F00000
        rsv = [r for r in rsv if r[0] != blob_addr]
        _, strings_end = fdt_serialize(root, rsv, cpuid)
        actual = (strings_end + 5 * 16 + 0xFFF) & ~0xFFF
        rsv.append((blob_addr, actual))
        fdt, _ = fdt_serialize(root, rsv, cpuid, totalsize=actual)
        if blob_addr + len(fdt) > entry:
            raise SystemExit('FDT runs into the image')
        open(os.path.join(a.out, 'fdt.dtb'), 'wb').write(fdt)
        segs.append((blob_addr, fdt))
    else:
        entry = 0x00200000
        info['x'] = [int(a.x0, 0), 0, 0, 0]
        segs.append((entry, image))

    info['entry'] = entry
    info['regs'] = len(regs)

    # RAM file
    ram = os.path.join(a.out, 'ram.bin')
    with open(ram, 'wb') as f:
        f.truncate(ram_bytes)
        for addr, data in segs:
            if addr + len(data) > ram_bytes:
                raise SystemExit('segment 0x%x+0x%x outside RAM' % (addr, len(data)))
            f.seek(addr)
            f.write(data)
    with open(os.path.join(a.out, 'regs-init.txt'), 'w') as f:
        f.write('# register state at hand-over (%s, %d MB), see mkboot.py\n' % (a.scenario, a.ram_mb))
        for k in sorted(regs):
            f.write('0x%08x 0x%08x\n' % (k, regs[k]))
    with open(os.path.join(a.out, 'scenario.json'), 'w') as f:
        json.dump(info, f, indent=1)
    print(json.dumps(info))


if __name__ == '__main__':
    main()

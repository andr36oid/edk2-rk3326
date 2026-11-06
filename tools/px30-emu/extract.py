#!/usr/bin/env python3
"""Read the results of an andr36oid-px30 run out of its RAM and register files.

  extract.py <outdir> [--symbols Build/r36sPkg/DEBUG_CLANGPDB] [--pc HEX ...]

Writes into <outdir>:
  log.txt, log-prev.txt   the firmware's RAM log ring (RkLogLib, 0x01E00000:
                          this boot, +0x80000: the previous one), oldest first
  screen.png              what the VOP scans out (VOPB, else VOPL; WIN1), or
                          the framebuffer the log header names
  trace-summary.txt       unhandled register accesses grouped by block and
                          address, with the first PCs (symbolized)
  stop.txt                PC/LR from monitor.txt, symbolized
  drivers.txt             each DXE driver: loaded, entry point failed, or
                          never dispatched (depex), from the DEBUG log
--pc symbolizes extra addresses. Symbols: SEC from FV/FVMAIN_COMPACT.Fv.map,
DXE drivers from the "Loading driver at ... <name>.efi" lines in the log and
llvm-symbolizer on the module's .dll (CLANGPDB, PDB next to it).
"""
import argparse
import bisect
import glob
import json
import os
import re
import struct
import subprocess
import sys

LOG_BASE = 0x01E00000
LOG_HALF = 0x00080000


def read_ram(path, addr, size):
    with open(path, 'rb') as f:
        f.seek(addr)
        return f.read(size)


def read_log(ram, base):
    hdr = read_ram(ram, base, 72)
    if len(hdr) < 72:
        return None, {}
    magic, hsize, written, dsize, fbvalid, fbbase, fbw, fbh, fbs, fbbpp, col, row, scroff, handed, uartdead, _ = \
        struct.unpack('<IIQIIQ10I', hdr)
    info = dict(magic=hex(magic), header=hsize, written=written, datasize=dsize, fbvalid=fbvalid,
                fbbase=hex(fbbase), fbw=fbw, fbh=fbh, fbstride=fbs, fbbpp=fbbpp, screenoff=scroff,
                handedoff=handed, uartdead=uartdead)
    if magic != 0x474C4B52 or dsize == 0 or dsize > LOG_HALF:
        return None, info
    data = read_ram(ram, base + hsize, dsize)
    if written <= dsize:
        text = data[:written]
    else:
        pos = written % dsize
        text = data[pos:] + data[:pos]
    return text.decode('latin-1'), info


def u32(regs, addr):
    off = addr - 0xFF000000
    return struct.unpack_from('<I', regs, off)[0]


def render(ram, regs, info, out_png):
    try:
        from PIL import Image
    except ImportError:
        return 'PIL missing, no PNG'
    src = None
    for vop, name in ((0xFF460000, 'VOPB'), (0xFF470000, 'VOPL')):
        ctrl = u32(regs, vop + 0x90)
        if ctrl & 1 and not (u32(regs, vop + 0x18) & 2):
            dsp = u32(regs, vop + 0xA4)
            w, h = (dsp & 0xFFFF) + 1, (dsp >> 16) + 1
            fmt = (ctrl >> 4) & 7
            rbswap = (ctrl >> 12) & 1
            stride = (u32(regs, vop + 0x98) & 0x1FFF) * 4
            base = u32(regs, vop + 0xA0)
            src = (name, base, w, h, stride, fmt, rbswap)
            break
    if src is None:
        if info.get('fbvalid'):
            bpp = info['fbbpp']
            src = ('log header', int(info['fbbase'], 16), info['fbw'], info['fbh'], info['fbstride'],
                   {4: 0, 3: 1, 2: 2}.get(bpp, 0), 0)
        else:
            return 'no window enabled on VOPB/VOPL (screen dark)'
    name, base, w, h, stride, fmt, rbswap = src
    bpp = {0: 4, 1: 3, 2: 2}.get(fmt)
    if bpp is None or w > 4096 or h > 4096 or w == 0 or h == 0:
        return '%s: format %d %dx%d not rendered' % (name, fmt, w, h)
    raw = read_ram(ram, base, stride * h)
    img = Image.new('RGB', (w, h))
    px = []
    for y in range(h):
        line = raw[y * stride: y * stride + w * bpp]
        for x in range(w):
            p = line[x * bpp:(x + 1) * bpp]
            if len(p) < bpp:
                px.append((0, 0, 0))
                continue
            if bpp == 2:
                v = p[0] | (p[1] << 8)
                r, g, b = ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31
            else:
                # VOP: without rb_swap the bytes are B, G, R(, A) (DRM XRGB8888 /
                # RGB888, kernel rockchip_drm_vop.c has_rb_swapped)
                b, g, r = p[0], p[1], p[2]
            if rbswap:
                r, b = b, r
            px.append((r, g, b))
    img.putdata(px)
    img.save(out_png)
    return '%s WIN1 %dx%d fmt %s%s stride %d at 0x%08x -> %s' % (
        name, w, h, {0: 'ARGB8888', 1: 'RGB888', 2: 'RGB565'}[fmt], ' rb_swap' if rbswap else '',
        stride, base, os.path.basename(out_png))


EFI_KEYS = {}   # (SizeOfImage, AddressOfEntryPoint, SizeOfCode) -> module


def index_efi(build):
    """Key the build's .efi files by header fields that relocation does not
    change, to name images in RAM that carry no PDB path (RELEASE)."""
    if not build or not os.path.isdir(build):
        return
    for f in glob.glob(os.path.join(build, 'AARCH64', '**', 'OUTPUT', '*.efi'), recursive=True):
        try:
            h = open(f, 'rb').read(0x400)
            e = struct.unpack_from('<I', h, 0x3C)[0]
            opt = e + 24
            if h[:2] != b'MZ' or h[e:e + 4] != b'PE\0\0':
                continue
            key = (struct.unpack_from('<I', h, opt + 56)[0], struct.unpack_from('<I', h, opt + 16)[0],
                   struct.unpack_from('<I', h, opt + 4)[0])
            EFI_KEYS.setdefault(key, os.path.basename(f)[:-4])
        except (OSError, struct.error):
            pass


def pe_at(ram, base):
    """(SizeOfImage, module name from the CodeView PDB path) of a PE image
    loaded at base, else None."""
    h = read_ram(ram, base, 0x400)
    if len(h) < 0x40 or h[:2] != b'MZ':
        return None
    e = struct.unpack_from('<I', h, 0x3C)[0]
    if e > 0x300 or h[e:e + 4] != b'PE\0\0':
        return None
    opt = e + 24
    if struct.unpack_from('<H', h, opt)[0] != 0x20B:
        return None
    size = struct.unpack_from('<I', h, opt + 56)[0]
    nrva = struct.unpack_from('<I', h, opt + 108)[0]
    name = '?'
    key = (size, struct.unpack_from('<I', h, opt + 16)[0], struct.unpack_from('<I', h, opt + 4)[0])
    if key in EFI_KEYS:
        name = EFI_KEYS[key]
    if nrva > 6:
        drva, dsize = struct.unpack_from('<II', h, opt + 112 + 6 * 8)
        if drva and dsize and drva < size:
            d = read_ram(ram, base + drva, 28)
            if len(d) == 28:
                ptr = struct.unpack_from('<I', d, 20)[0]
                cv = read_ram(ram, base + ptr, 300) if ptr < size else b''
                if cv[:4] == b'RSDS':
                    path = cv[24:].split(b'\0')[0].decode('latin-1')
                    name = re.split(r'[\\/]', path)[-1].rsplit('.', 1)[0]
    return size, name


def image_for(ram, pc, span=0x400000):
    """Walk back from pc page by page to the PE image that contains it
    (the loader puts images on 4 KiB boundaries)."""
    if not ram:
        return None
    a = pc & ~0xFFF
    while a >= 0 and pc - a < span:
        r = pe_at(ram, a)
        if r and pc < a + r[0]:
            return a, r[1]
        a -= 0x1000
    return None


class Symbols:
    def __init__(self, build, logtext, ram=None):
        self.build = build
        self.ram = ram
        self.sec = []
        self.mods = []          # (base, name)
        if build and os.path.isdir(build):
            m = os.path.join(build, 'FV', 'FVMAIN_COMPACT.Fv.map')
            if os.path.exists(m):
                for line in open(m):
                    mm = re.match(r'\s+0x([0-9a-fA-F]+)\s+(\S+)', line)
                    if mm:
                        self.sec.append((int(mm.group(1), 16), mm.group(2)))
                self.sec.sort()
        for mm in re.finditer(r'Loading (?:driver|DxeCore|PEIM|module) at 0x([0-9A-Fa-f]+) EntryPoint=0x([0-9A-Fa-f]+) (\S+)\.efi',
                              logtext or ''):
            self.mods.append((int(mm.group(1), 16), mm.group(3)))
        self.mods.sort()
        self.dlls = {}
        if build and os.path.isdir(build):
            for d in glob.glob(os.path.join(build, 'AARCH64', '**', 'DEBUG', '*.dll'), recursive=True):
                self.dlls.setdefault(os.path.basename(d)[:-4], d)

    def from_map(self, dll, rva):
        """Nearest symbol from the lld-link map next to the .dll (RELEASE
        builds have no PDB line info)."""
        mp = dll[:-4] + '.map'
        if not os.path.exists(mp):
            return '??'
        syms = []
        for line in open(mp, errors='replace'):
            m = re.match(r'^([0-9a-f]{8}) [0-9a-f]{8}\s+0\s+(\S+)\s*$', line)
            if m:
                syms.append((int(m.group(1), 16), m.group(2)))
        syms.sort()
        j = bisect.bisect_right([a for a, _ in syms], rva) - 1
        return '%s+0x%x (map)' % (syms[j][1], rva - syms[j][0]) if j >= 0 else '??'

    def __call__(self, pc):
        if pc is None:
            return '?'
        # DXE module: from the image headers in RAM (works for RELEASE too),
        # else from the "Loading driver" lines
        img = image_for(self.ram, pc) if 0x02100000 <= pc < 0x40000000 else None
        i = bisect.bisect_right([b for b, _ in self.mods], pc) - 1
        if img or i >= 0:
            base, name = img if img else self.mods[i]
            off = pc - base
            if off < 0x200000:
                dll = self.dlls.get(name)
                if dll:
                    try:
                        r = subprocess.run(['llvm-symbolizer-19', '--obj=' + dll, hex(off)],
                                           capture_output=True, text=True, timeout=20).stdout.split('\n')
                        fn, loc = r[0], (r[1] if len(r) > 1 else '')
                        if fn in ('??', ''):
                            fn, loc = self.from_map(dll, off), ''
                        loc = re.sub(r'^.*?/(Mu_Basecore|Silicon|Platforms|Common|Silicium-ACPI)/', r'\1/', loc)
                        return '%s+0x%x %s %s' % (name, off, fn, loc)
                    except Exception:
                        pass
                return '%s+0x%x' % (name, off)
        if self.sec and self.sec[0][0] <= pc < 0x02100000:
            j = bisect.bisect_right([a for a, _ in self.sec], pc) - 1
            if j >= 0:
                return 'Sec %s+0x%x' % (self.sec[j][1], pc - self.sec[j][0])
        return '0x%x' % pc


def drivers(build, text, out):
    """One line per DXE driver: loaded / start failed / never dispatched."""
    names = {}
    if build and os.path.isdir(build):
        for d in glob.glob(os.path.join(build, 'FV', 'Ffs', '*')):
            b = os.path.basename(d)
            if len(b) > 36 and b[8] == '-':
                names[b[:36].upper()] = b[36:]
    rows = []
    loaded = {}
    for m in re.finditer(r'Loading (driver|DxeCore) at 0x([0-9A-Fa-f]+) EntryPoint=0x[0-9A-Fa-f]+[ \t]*(\S*)',
                         text or ''):
        n = re.sub(r'\.efi$', '', m.group(3)) or m.group(1)
        loaded[int(m.group(2), 16)] = n
        rows.append([n, 'loaded at 0x%x' % int(m.group(2), 16)])
    for m in re.finditer(r'Error: Image at 0*([0-9A-Fa-f]+) start failed: (.*)', text or ''):
        n = loaded.get(int(m.group(1), 16), m.group(1))
        for r in rows:
            if r[0] == n:
                r[1] += ', entry point FAILED: ' + m.group(2).strip()
    for m in re.finditer(r'Driver ([0-9A-Fa-f-]{36}) was discovered but not loaded', text or ''):
        g = m.group(1).upper()
        rows.append([names.get(g, g), 'NOT DISPATCHED (dependency expression never satisfied), ' + g])
    with open(out, 'w') as o:
        for n, what in rows:
            o.write('%-40s %s\n' % (n, what))
    return rows


def summarize_trace(path, sym, out):
    groups = {}
    order = []
    notes = []
    head = []
    if not os.path.exists(path):
        return
    with open(path, errors='replace') as f:
        for line in f:
            if line.startswith('#'):
                head.append(line.rstrip())
                continue
            if line.startswith('NOTE'):
                notes.append(line.rstrip())
                continue
            m = re.match(r'([rw])(\d) ([0-9a-f]+) ([0-9a-f]+) pc=([0-9a-f]+) lr=([0-9a-f]+) (\S+)( UNHANDLED)?', line)
            if not m or not m.group(8):
                continue
            key = (m.group(7), int(m.group(3), 16), m.group(1))
            g = groups.get(key)
            if g is None:
                g = groups[key] = {'n': 0, 'pcs': [], 'vals': set()}
                order.append(key)
            g['n'] += 1
            pc = int(m.group(5), 16)
            if pc not in g['pcs'] and len(g['pcs']) < 3:
                g['pcs'].append(pc)
            if len(g['vals']) < 6:
                g['vals'].add(m.group(4))
    with open(out, 'w') as o:
        o.write('\n'.join(head) + '\n\n')
        o.write('Unhandled register accesses (stored like RAM): %d addresses\n' % len(order))
        o.write('block      addr      rw  count  values                      first PCs\n')
        for key in order:
            g = groups[key]
            o.write('%-10s %08x  %s  %5d  %-27s %s\n' % (
                key[0], key[1], key[2], g['n'], ','.join(sorted(g['vals']))[:27],
                ' | '.join(sym(p) for p in g['pcs'])))
        if notes:
            o.write('\nNotes from the model (%d):\n' % len(notes))
            for n in notes[:400]:
                o.write(n + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir')
    ap.add_argument('--symbols')
    ap.add_argument('--pc', action='append', default=[])
    a = ap.parse_args()
    out = a.outdir
    ram = os.path.join(out, 'ram.bin')
    regs = open(os.path.join(out, 'regs.bin'), 'rb').read() if os.path.exists(os.path.join(out, 'regs.bin')) else b''

    text, info = read_log(ram, LOG_BASE)
    prev, pinfo = read_log(ram, LOG_BASE + LOG_HALF)
    open(os.path.join(out, 'log.txt'), 'w').write(text or '(no log ring)\n')
    open(os.path.join(out, 'log-prev.txt'), 'w').write(prev or '(no previous log)\n')
    index_efi(a.symbols)
    sym = Symbols(a.symbols, text, ram)

    screen = render(ram, regs, info, os.path.join(out, 'screen.png')) if regs else 'no regs.bin'

    stop = []
    mon = os.path.join(out, 'monitor.txt')
    if os.path.exists(mon):
        mt = open(mon).read()
        pc = re.search(r'PC=([0-9a-fA-F]+)', mt)
        lr = re.search(r'X30=([0-9a-fA-F]+)', mt)
        el = re.search(r'EL(\d)', mt)
        if pc:
            stop.append('PC  %s  %s' % (pc.group(1), sym(int(pc.group(1), 16))))
        if lr:
            stop.append('LR  %s  %s' % (lr.group(1), sym(int(lr.group(1), 16))))
        if el:
            stop.append('EL%s' % el.group(1))
        # frame-pointer walk: [x29] = caller's x29, [x29 + 8] = return address
        fp = re.search(r'X29=([0-9a-fA-F]+)', mt)
        fp = int(fp.group(1), 16) if fp else 0
        for depth in range(20):
            if not fp or fp & 7 or fp >= os.path.getsize(ram):
                break
            nfp, ret = struct.unpack('<QQ', read_ram(ram, fp, 16))
            if not ret:
                break
            stop.append('#%-2d %010x  %s' % (depth + 1, ret, sym(ret)))
            if nfp <= fp:
                break
            fp = nfp
    for p in a.pc:
        stop.append('%s  %s' % (p, sym(int(p, 16))))
    open(os.path.join(out, 'stop.txt'), 'w').write('\n'.join(stop) + '\n')

    summarize_trace(os.path.join(out, 'trace.txt'), sym, os.path.join(out, 'trace-summary.txt'))
    rows = drivers(a.symbols, text, os.path.join(out, 'drivers.txt'))

    lines = (text or '').rstrip('\n').split('\n')
    print('log ring: %s, %d bytes, last lines:' % (info.get('magic'), info.get('written', 0)))
    for l in lines[-12:]:
        print('  | ' + l)
    bad = [r for r in rows if 'FAILED' in r[1] or 'NOT DISPATCHED' in r[1]]
    print('drivers: %d loaded, %d failed or not dispatched%s' % (
        sum(1 for r in rows if r[1].startswith('loaded')), len(bad),
        ''.join('\n  ! %s: %s' % (n, w) for n, w in bad)))
    print('screen: ' + screen)
    for s in stop:
        print('stop: ' + s)
    json.dump({'log': info, 'prev': pinfo, 'screen': screen, 'stop': stop},
              open(os.path.join(out, 'extract.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# Second opinion on RkDisplayDxe's FDT parser: read each DTB here with a
# separate, plain Python FDT reader, apply U-Boot's defaults, and compare
# with what the C parser reports (test_display --dump).
#
#   check-display-dtbs.py ./test_display a.dtb b.dtb ...
#
# SPDX-License-Identifier: BSD-2-Clause-Patent

import json
import struct
import subprocess
import sys

DEFAULT_FLAGS = 1 | 2 | 64 | 512 | 2048   # VIDEO | BURST | HBP | EOT_PACKET | LPM


def parse(path):
    b = open(path, 'rb').read()
    magic, _, off_struct, off_strings, _, _, _, _, sz_str, _ = struct.unpack('>10I', b[:40])
    assert magic == 0xd00dfeed
    strs = b[off_strings:off_strings + sz_str]
    p, stack, root, phandles = off_struct, [], None, {}
    while True:
        tok = struct.unpack('>I', b[p:p + 4])[0]
        p += 4
        if tok == 1:
            e = b.index(b'\0', p)
            n = {'name': b[p:e].decode(), 'props': {}, 'children': [], 'parent': stack[-1] if stack else None}
            p = (e + 4) & ~3
            if stack:
                stack[-1]['children'].append(n)
            else:
                root = n
            stack.append(n)
        elif tok == 2:
            stack.pop()
        elif tok == 3:
            ln, no = struct.unpack('>II', b[p:p + 8])
            p += 8
            name = strs[no:strs.index(b'\0', no)].decode()
            stack[-1]['props'][name] = b[p:p + ln]
            p = (p + ln + 3) & ~3
            if name == 'phandle':
                phandles[struct.unpack('>I', stack[-1]['props'][name])[0]] = stack[-1]
        elif tok == 4:
            continue
        elif tok == 9:
            return root, phandles


def u32s(v):
    return list(struct.unpack('>%dI' % (len(v) // 4), v[:len(v) // 4 * 4]))


def strs(v):
    return v.rstrip(b'\0').decode(errors='replace').split('\0')


def okay(n):
    st = n['props'].get('status')
    return st is None or strs(st)[0] in ('okay', 'ok')


def walk(n):
    yield n
    for c in n['children']:
        yield from walk(c)


def compat(n, c):
    return c in strs(n['props'].get('compatible', b''))


def u32(n, name, default):
    v = n['props'].get(name)
    return u32s(v)[0] if v is not None and len(v) >= 4 else default


def gpio(node, name, ph):
    v = node['props'].get(name)
    if v is None or len(v) < 12:
        return [0, 0, 0, 0]
    c = u32s(v)
    ctrl = ph[c[0]]
    bank = int(ctrl['name'].split('@')[0].replace('gpio', ''))
    return [1, bank, c[1], c[2] & 1]


def supply(root, node, name, ph):
    v = node['props'].get(name)
    if v is None:
        return [0, '', 0, 0, '']
    handle = u32s(v)[0]
    r = ph[handle]
    nm = strs(r['props']['regulator-name'])[0] if 'regulator-name' in r['props'] else r['name']
    # another enabled user of the rail (not this consumer, not the backlight)
    shared = ''
    for n in walk(root):
        if n is node or not okay(n) or compat(n, 'pwm-backlight'):
            continue
        if any(k.endswith('-supply') and len(k) > 7 and len(pv) == 4 and u32s(pv)[0] == handle
               for k, pv in n['props'].items()):
            shared = n['name']
            break
    return [1, nm, u32(r, 'regulator-min-microvolt', 0), 1 if compat(r, 'regulator-fixed') else 0, shared]


def expected(path):
    root, ph = parse(path)
    # display-subsystem route: the okay one whose remote is the DSI host
    crtc = 0
    for n in walk(root):
        if n['name'] == 'route' and n['parent'] and n['parent']['name'] == 'display-subsystem':
            for r in n['children']:
                if okay(r) and 'connect' in r['props']:
                    ep = ph[u32s(r['props']['connect'])[0]]
                    remote = ph[u32s(ep['props']['remote-endpoint'])[0]]
                    up = remote
                    while up and not compat(up, 'rockchip,px30-mipi-dsi'):
                        up = up['parent']
                    if up:
                        crtc = u32(remote, 'reg', 0)
                        break
    dsi = [n for n in walk(root) if compat(n, 'rockchip,px30-mipi-dsi') and okay(n)][0]
    panel = [c for c in dsi['children'] if okay(c) and (compat(c, 'simple-panel-dsi') or compat(c, 'simple-panel'))][0]
    p = panel['props']
    out = {'panel': panel['name'], 'crtc': crtc,
           'lanes': u32(panel, 'dsi,lanes', 4), 'format': u32(panel, 'dsi,format', 0),
           'flags': u32(panel, 'dsi,flags', DEFAULT_FLAGS), 'channel': u32(panel, 'reg', 0),
           'lane_rate': u32(dsi, 'rockchip,lane-rate', 0),
           'delays': [u32(panel, k, 0) for k in ('prepare-delay-ms', 'reset-delay-ms', 'init-delay-ms',
                                                  'enable-delay-ms', 'disable-delay-ms', 'unprepare-delay-ms')],
           'bus_format': u32(panel, 'bus-format', 0x100e), 'power_invert': 1 if 'power-invert' in p else 0}

    tim = [c for c in panel['children'] if c['name'] == 'display-timings'][0]
    t = [c for c in tim['children'] if c['name'] == 'timing']
    t = t[0] if t else ph.get(u32(tim, 'native-mode', 0xffffffff))
    if t is None or 'hactive' not in t['props']:
        t = [c for c in tim['children'] if 'hactive' in c['props']][0]
    out['timing'] = [u32(t, k, -1) for k in ('clock-frequency', 'hactive', 'hfront-porch', 'hsync-len', 'hback-porch',
                                              'vactive', 'vfront-porch', 'vsync-len', 'vback-porch',
                                              'hsync-active', 'vsync-active', 'pixelclk-active')]
    out['ntimings'] = len(tim['children'])

    seq = p.get('panel-init-sequence', b'')
    i = cnt = 0
    while len(seq) - i > 3:
        i += 3 + seq[i + 2]
        cnt += 1
    out['init_cmds'] = cnt
    out['init_len'] = len(seq)
    out['exit_len'] = len(p.get('panel-exit-sequence', b''))
    out['reset'] = gpio(panel, 'reset-gpios', ph)
    out['enable'] = gpio(panel, 'enable-gpios', ph)
    out['power'] = supply(root, panel, 'power-supply', ph)
    out['blpower'] = supply(root, panel, 'backlight-supply', ph)

    bl = ph[u32s(p['backlight'])[0]]
    bp = bl['props']
    pw = u32s(bp['pwms'])
    pwm = ph[pw[0]]
    base = u32s(pwm['props']['reg'])[1]
    clocks = u32s(pwm['props']['clocks'])
    block = {34: 0, 35: 1}.get(clocks[1], 1 if base >= 0xff208000 else 0)
    index = u32(bl, 'default-brightness-level', 255)
    lv = u32s(bp['brightness-levels']) if 'brightness-levels' in bp else []
    if len(lv) > index:
        default, mx = lv[index], max(lv[-1], lv[0]) if lv[0] > lv[-1] else lv[-1]
    else:
        default, mx = index, 255
    pins = 0
    names = strs(pwm['props'].get('pinctrl-names', b''))
    if 'active' in names:
        for g in u32s(pwm['props']['pinctrl-%d' % names.index('active')]):
            pins += len(u32s(ph[g]['props']['rockchip,pins'])) // 4
    out['bl'] = [1, hex(base), block, pw[1], pw[2], 1 if len(pw) > 3 and pw[3] else 0, default, mx,
                 1 if 'enable-gpios' in bp else 0, 1 if 'power-supply' in bp else 0, min(pins, 4)]
    return out


def main():
    exe, files = sys.argv[1], sys.argv[2:]
    bad = 0
    for f in files:
        got = json.loads(subprocess.run([exe, '--dump', f], capture_output=True, text=True, check=True).stdout)
        exp = expected(f)
        diffs = [k for k in exp if got.get(k) != exp[k]]
        if got.get('status') != 0 or diffs:
            bad += 1
            print('MISMATCH', f, got.get('status'), {k: (got.get(k), exp[k]) for k in diffs})
    print('parser vs independent reader: %d DTBs, %d mismatches' % (len(files), bad))
    sys.exit(1 if bad else 0)


main()

#!/usr/bin/env python3
"""Check a card image after a run: what the firmware wrote to BOOT.

  cardcheck.py <card.img> <run outdir>

Finds partition 1 (BOOT) in the GPT, runs fsck.fat -n on a carved copy, lists
BOOT, and reads uefi-log.txt (and uefi-log-prev.txt) back with mtools. The
lines of uefi-log.txt must all be in the firmware's RAM log ring as the run
left it (<outdir>/log.txt or log-prev.txt from extract.py), in order, so a
file that went through the DW-MMC write path wrong (lost, shifted, or
garbled sectors) shows up. Prints a short report; exit status 0 = all good.
"""
import os
import struct
import subprocess
import sys
import tempfile

ENV = dict(os.environ, PATH=os.environ.get('PATH', '') + ':/usr/sbin:/sbin', MTOOLS_SKIP_CHECK='1')


def boot_partition(img):
    with open(img, 'rb') as f:
        f.seek(512)
        hdr = f.read(92)
        if hdr[:8] != b'EFI PART':
            raise SystemExit('no GPT in %s' % img)
        lba, n, size = struct.unpack_from('<QII', hdr, 72)
        f.seek(lba * 512)
        e = f.read(size)
        first, last = struct.unpack_from('<QQ', e, 32)
        return first * 512, (last - first + 1) * 512


def mtools(cmd, img, off, *args):
    r = subprocess.run([cmd, '-i', '%s@@%d' % (img, off)] + list(args), capture_output=True, env=ENV)
    return r.returncode, r.stdout


def in_order(lines, ring):
    """index of the first line of `lines` that is not found after the previous one"""
    pos = 0
    for i, l in enumerate(lines):
        j = ring.find(l, pos)
        if j < 0:
            return i
        pos = j + len(l)
    return None


def main():
    img, out = sys.argv[1], sys.argv[2]
    name = os.path.basename(img)
    ok = True
    off, size = boot_partition(img)
    print('== %s: BOOT at %d (+%d MB)' % (name, off, size >> 20))

    with tempfile.NamedTemporaryFile(dir=os.path.dirname(os.path.abspath(out)) or '.', suffix='.fat') as t:
        subprocess.run(['dd', 'if=' + img, 'of=' + t.name, 'bs=1M', 'iflag=skip_bytes,count_bytes',
                        'skip=%d' % off, 'count=%d' % size, 'conv=sparse', 'status=none'], check=True)
        r = subprocess.run(['fsck.fat', '-n', '-v', t.name], capture_output=True, env=ENV, text=True)
        tail = [l for l in r.stdout.splitlines() + r.stderr.splitlines() if l.strip()][-3:]
        print('%s fsck.fat -n: rc %d: %s' % ('ok  ' if r.returncode == 0 else 'FAIL', r.returncode,
                                             ' | '.join(tail)))
        ok &= r.returncode == 0

    rc, listing = mtools('mdir', img, off, '-b', '::')
    files = [l.split('/')[-1] for l in listing.decode(errors='replace').split()]
    print('     BOOT: %s' % ' '.join(files))

    ring = ''
    for f in ('log.txt', 'log-prev.txt'):
        p = os.path.join(out, f)
        if os.path.exists(p):
            ring += open(p, encoding='utf-8', errors='replace', newline='').read().replace('\r', '') + '\n'

    for f in ('uefi-log.txt', 'uefi-log-prev.txt'):
        rc, data = mtools('mtype', img, off, '::' + f)
        if rc != 0:
            print('%s BOOT/%s not written' % ('FAIL' if f == 'uefi-log.txt' else '    ', f))
            ok &= f != 'uefi-log.txt'
            continue
        text = data.decode('utf-8', errors='replace').replace('\r', '')
        lines = [l for l in text.split('\n') if l.strip()]
        bad = [l for l in lines if any(ord(c) < 9 or 13 < ord(c) < 32 or c == '�' for c in l)]
        miss = in_order(lines, ring) if ring else None
        print('%s BOOT/%s: %d bytes, %d lines, %d with junk bytes, %s' % (
            'ok  ' if not bad and miss is None else 'FAIL', f, len(data), len(lines), len(bad),
            'every line in the RAM log in order' if miss is None else
            'line %d not in the RAM log: %r' % (miss + 1, lines[miss][:100])))
        ok &= not bad and (miss is None or f != 'uefi-log.txt')
        if lines:
            print('     last line: %s' % lines[-1][:120])
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
#
# Check that every DXE driver in the r36s firmware volume can be dispatched:
# each GUID a dependency expression waits for must be a protocol that a module
# in the same FV installs (PRODUCERS, each checked against that module's
# source), and walking the dispatch with those must reach every driver.
#
# Why: Mu's NonCoherentDmaLib links IoMmuLib, whose AARCH64 [Depex] waits for
# gEdkiiIoMmuProtocolGuid. Nothing on this SoC installs it, so the SD/eMMC
# and USB host drivers were never started and the build gave no sign of it.
#
# Reads FVMAIN.Fv itself (the FV packed into the FD), not the build's
# per-module files.
#
# usage: check-depex.py <tree root> <Build/r36sPkg/<TARGET>_CLANGPDB>
#
# SPDX-License-Identifier: BSD-2-Clause-Patent
#

import glob
import os
import re
import sys
import uuid

PRODUCERS = {
    'gEfiPcdProtocolGuid': 'PcdDxe',
    'gEfiCpuArchProtocolGuid': 'ArmCpuDxe',
    'gHardwareInterruptProtocolGuid': 'ArmGicDxe',
    'gEfiTimerArchProtocolGuid': 'ArmTimerDxe',
    'gEfiMetronomeArchProtocolGuid': 'MetronomeDxe',
    'gEfiMonotonicCounterArchProtocolGuid': 'EmbeddedMonotonicCounter',
    'gEfiRealTimeClockArchProtocolGuid': 'RealTimeClock',
    'gEfiResetArchProtocolGuid': 'ResetSystemRuntimeDxe',
    'gEfiRuntimeArchProtocolGuid': 'RuntimeDxe',
    'gEfiSecurityArchProtocolGuid': 'SecurityStubDxe',
    'gEfiBdsArchProtocolGuid': 'BdsDxe',
    'gEfiVariableArchProtocolGuid': 'VariableRuntimeDxe',
    'gEfiVariableWriteArchProtocolGuid': 'VariableRuntimeDxe',
    'gEdkiiVariablePolicyProtocolGuid': 'VariableRuntimeDxe',
    'gEfiWatchdogTimerArchProtocolGuid': 'WatchdogTimer',
    'gEfiCapsuleArchProtocolGuid': 'CapsuleRuntimeDxe',
    'gEfiRscHandlerProtocolGuid': 'ReportStatusCodeRouterRuntimeDxe',
    'gEfiHiiDatabaseProtocolGuid': 'HiiDatabase',
    'gEfiHiiStringProtocolGuid': 'HiiDatabase',
    'gEfiHiiConfigRoutingProtocolGuid': 'HiiDatabase',
}

# A DXE driver without a depex waits for all of these (DxeCore
# CoreAllEfiServicesAvailable, DxeProtocolNotify.c).
ARCH = [
    'gEfiSecurityArchProtocolGuid', 'gEfiCpuArchProtocolGuid', 'gEfiMetronomeArchProtocolGuid',
    'gEfiTimerArchProtocolGuid', 'gEfiBdsArchProtocolGuid', 'gEfiWatchdogTimerArchProtocolGuid',
    'gEfiRuntimeArchProtocolGuid', 'gEfiVariableArchProtocolGuid', 'gEfiVariableWriteArchProtocolGuid',
    'gEfiCapsuleArchProtocolGuid', 'gEfiMonotonicCounterArchProtocolGuid', 'gEfiResetArchProtocolGuid',
    'gEfiRealTimeClockArchProtocolGuid',
]

DISPATCHED_TYPES = ('DXE_DRIVER', 'DXE_RUNTIME_DRIVER', 'UEFI_DRIVER')


def guid_names(root):
    names = {}
    pat = re.compile(r'\s*(g\w+)\s*=\s*\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*\{([^}]*)\}')
    for dec in glob.glob(os.path.join(root, '**', '*.dec'), recursive=True):
        with open(dec, errors='replace') as f:
            for line in f:
                m = pat.match(line)
                if not m:
                    continue
                b = [int(x, 16) for x in m.group(5).split(',')]
                g = uuid.UUID(fields=(int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16),
                                      b[0], b[1], int.from_bytes(bytes(b[2:]), 'big')))
                names.setdefault(str(g), m.group(1))
    return names


def decode(data):
    ops, i = [], 0
    while i < len(data):
        op = data[i]
        i += 1
        if op in (0, 1, 2):         # BEFORE, AFTER, PUSH + GUID
            ops.append((op, str(uuid.UUID(bytes_le=bytes(data[i:i + 16])))))
            i += 16
        elif op == 8:               # END
            break
        else:
            ops.append((op, None))
    return ops


def evaluate(ops, have, names):
    stack = []
    for op, g in ops:
        if op in (0, 1):            # BEFORE/AFTER: ordering only
            return True
        if op == 2:
            stack.append(names.get(g, g) in have)
        elif op == 3:
            stack.append(stack.pop() & stack.pop())
        elif op == 4:
            stack.append(stack.pop() | stack.pop())
        elif op == 5:
            stack.append(not stack.pop())
        elif op == 6:
            stack.append(True)
        elif op == 7:
            stack.append(False)
        elif op == 9:               # SOR: runs only when something asks for it
            return False
    return bool(stack) and stack[-1]


def walk_sections(data, out):
    i = 0
    while i + 4 <= len(data):
        size = int.from_bytes(data[i:i + 3], 'little')
        stype = data[i + 3]
        hdr = 4
        if size == 0xFFFFFF:
            size = int.from_bytes(data[i + 4:i + 8], 'little')
            hdr = 8
        if size < hdr or i + size > len(data):
            break
        body = data[i + hdr:i + size]
        if stype == 0x02:           # GUID-defined: data starts at DataOffset
            off = int.from_bytes(body[16:18], 'little') - hdr
            walk_sections(body[off:], out)
        else:
            out.setdefault(stype, body)
        i = (i + size + 3) & ~3


def fv_files(fv):
    # (name GUID, FFS type, {section type: body}) for each file in an FV image
    if fv[40:44] != b'_FVH':
        raise SystemExit('check-depex: not a firmware volume')
    length = int.from_bytes(fv[32:40], 'little')
    pos = int.from_bytes(fv[48:50], 'little')
    ext = int.from_bytes(fv[52:54], 'little')
    if ext:
        pos = ext + int.from_bytes(fv[ext + 16:ext + 20], 'little')
    files = []
    while True:
        pos = (pos + 7) & ~7
        if pos + 24 > length or fv[pos:pos + 24] == b'\xff' * 24:
            break
        name = str(uuid.UUID(bytes_le=bytes(fv[pos:pos + 16])))
        ftype, attr = fv[pos + 18], fv[pos + 19]
        size, hdr = int.from_bytes(fv[pos + 20:pos + 23], 'little'), 24
        if attr & 0x01:
            size, hdr = int.from_bytes(fv[pos + 24:pos + 32], 'little'), 32
        if size < hdr:
            break
        sections = {}
        if ftype not in (0xF0, 0x01):   # pad, raw
            walk_sections(fv[pos + hdr:pos + size], sections)
        files.append((name, ftype, sections, fv[pos + hdr:pos + size]))
        pos += size
    return files


def main():
    root, build = sys.argv[1], sys.argv[2]
    names = guid_names(root)
    fv = open(os.path.join(build, 'FV', 'FVMAIN.Fv'), 'rb').read()
    files = fv_files(fv)
    label = {}
    modules = {}
    apriori = []
    for name, ftype, sections, raw in files:
        ui = sections.get(0x15)
        label[name] = ui.decode('utf-16-le').rstrip('\0') if ui else name
        if name.upper() == 'FC510EE7-FFDC-11D4-BD41-0080C73C8881':
            data = sections.get(0x19, b'')
            apriori = [str(uuid.UUID(bytes_le=bytes(data[i:i + 16]))) for i in range(0, len(data) - len(data) % 16, 16)]
        if ftype != 0x07:               # EFI_FV_FILETYPE_DRIVER
            continue
        if 0x13 in sections:
            ops = decode(sections[0x13])
        else:                           # no depex: all architectural protocols
            ops = [(2, next((k for k, v in names.items() if v == g), g)) for g in ARCH]
            ops += [(3, None)] * (len(ARCH) - 1)
        modules[name] = ops

    if not modules:
        print('check-depex: no DXE drivers found under', build)
        return 1

    errors = 0
    modules = {label[g]: ops for g, ops in modules.items()}
    present = set(modules)
    for g, p in sorted(PRODUCERS.items()):
        if p not in present:
            print(f'check-depex: note: {g} comes from {p}, which is not in this FV')

    for name, ops in sorted(modules.items()):
        for op, g in ops:
            if op == 2 and names.get(g, g) not in PRODUCERS:
                print(f'check-depex: {name} waits for {names.get(g, g)} ({g}): nothing in PRODUCERS installs it')
                errors += 1

    # The a priori file's drivers are scheduled first, in its order, without
    # their depex being looked at (DxeCore Dispatcher.c, "RESULT = TRUE (Apriori)").
    have, pending = set(), dict(modules)
    for g in apriori:
        name = label.get(g)
        if name in pending:
            del pending[name]
            have |= {p for p, m in PRODUCERS.items() if m == name}

    progress = True
    while progress:
        progress = False
        for name in sorted(pending):
            if evaluate(pending[name], have, names):
                del pending[name]
                progress = True
                have |= {g for g, p in PRODUCERS.items() if p == name}
    for name in sorted(pending):
        missing = sorted({names.get(g, g) for op, g in pending[name] if op == 2 and names.get(g, g) not in have})
        print(f'check-depex: {name} is never dispatched, waiting for: {", ".join(missing) or "(order/SOR)"}')
        errors += 1
    if errors:
        print('check-depex: an incremental build keeps a .depex a module no longer has; '
              'build-mu.sh clears them first, a clean Build/ directory rules it out')

    if errors:
        return 1
    print(f'check-depex: {len(modules)} DXE drivers in FVMAIN.Fv ({len(apriori)} a priori), all dispatchable')
    return 0


if __name__ == '__main__':
    sys.exit(main())

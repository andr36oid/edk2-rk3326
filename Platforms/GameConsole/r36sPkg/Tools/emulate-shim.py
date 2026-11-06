#!/usr/bin/env python3
"""Run Mu-r36s.bin's BootShim in unicorn for both ways it is entered and check
that it reaches the FD's first instruction at the FD base with the FD intact:
  emulate-shim.py <Mu-r36s.bin> <fd_base> <fd_size>
1. U-Boot booti (chainload): image at fd_base - shim size, runs in place.
2. Miniloader uboot.img: image at 0x00200000, the shim copies the FD.
3. The old trap: image at fd_base itself (shim inside the copy destination)."""
import struct, sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UcError
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_X0

img = open(sys.argv[1], "rb").read()
fd_base, fd_size = int(sys.argv[2], 0), int(sys.argv[3], 0)
text_offset = struct.unpack_from("<Q", img, 8)[0]
payload = len(img) - fd_size
fd = img[payload:]

def run(load):
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    mu.mem_map(0, 64 << 20)
    mu.mem_write(load, img)
    mu.reg_write(UC_ARM64_REG_X0, 0x01F00000)
    hit = {}
    def hook(uc, addr, size, data):
        if addr == fd_base:
            hit["pc"] = addr
            uc.emu_stop()
    mu.hook_add(UC_HOOK_CODE, hook)
    try:
        mu.emu_start(load, 0, count=fd_size)   # the copy loop runs fd_size/16 x 4 times
    except UcError as e:
        return "fault: %s at 0x%x" % (e, mu.reg_read(UC_ARM64_REG_PC))
    if "pc" not in hit:
        return "never reached 0x%x (pc 0x%x)" % (fd_base, mu.reg_read(UC_ARM64_REG_PC))
    if bytes(mu.mem_read(fd_base, fd_size)) != fd:
        return "reached the FD base but the FD there is not intact"
    if mu.reg_read(UC_ARM64_REG_X0) != 0x01F00000:
        return "x0 changed"
    return "ok"

bad = 0
for name, load, want in (("booti in place", fd_base - payload, "ok"),
                         ("miniloader copy", 0x200000, "ok"),
                         ("loaded at the FD base (must not be used)", fd_base, None)):
    r = run(load)
    print("%-42s load 0x%08x: %s" % (name, load, r))
    if want and r != want:
        bad = 1
if fd_base - payload != text_offset:
    print("text_offset 0x%x != load address for in-place 0x%x" % (text_offset, fd_base - payload)); bad = 1
sys.exit(bad)

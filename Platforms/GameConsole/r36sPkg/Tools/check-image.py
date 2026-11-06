#!/usr/bin/env python3
"""Check Mu-r36s.bin's BootShim against the FD it carries:
  check-image.py <Mu-r36s.bin> <SILICIUM_UEFI.fd> <fd_base> <fd_size>
- the shim's adr target (where it copies or jumps from) is where the FD starts
- the FD bytes there equal the built FD (nothing between the shim and the FD)
- booti (flags bit 3) leaves the image where text_offset says, so the FD lands
  on its base without a copy when loaded at fd_base - shim size
- the miniloader load address (0x00200000) copies without overlapping the shim
Exit 1 with a reason on any mismatch."""
import struct, sys

def fail(msg):
    sys.exit("check-image: " + msg)

img = open(sys.argv[1], "rb").read()
fd = open(sys.argv[2], "rb").read()
fd_base, fd_size = int(sys.argv[3], 0), int(sys.argv[4], 0)

adr = struct.unpack_from("<I", img, 0)[0]
if (adr & 0x9F00001F) != 0x10000001:
    fail("first instruction is not adr x1 (0x%08x)" % adr)
imm = ((adr >> 5) & 0x7FFFF) << 2 | ((adr >> 29) & 3)
payload = imm
text_offset, image_size, flags = struct.unpack_from("<QQQ", img, 8)
if img[56:60] != b"ARMd":
    fail("no arm64 Image magic")
if payload % 16 or payload < 64:
    fail("payload offset 0x%x" % payload)
if len(fd) != fd_size or len(img) != payload + fd_size:
    fail("sizes: image 0x%x, payload 0x%x, fd 0x%x (want 0x%x)" % (len(img), payload, len(fd), fd_size))
if img[payload:] != fd:
    fail("the bytes at the shim's payload offset 0x%x are not the FD" % payload)
if text_offset + payload != fd_base:
    fail("text_offset 0x%x + shim 0x%x != FD base 0x%x" % (text_offset, payload, fd_base))
if image_size != payload + fd_size:
    fail("image_size 0x%x, want 0x%x" % (image_size, payload + fd_size))
if not flags & 8:
    fail("flags bit 3 not set")

# U-Boot booti_setup (cmd/booti.c): dst = ALIGN(ep - text_offset, 2M) + text_offset
ep = fd_base - payload
dst = ((ep - text_offset + 0x1FFFFF) & ~0x1FFFFF) + text_offset
if dst != ep:
    fail("booti would move the image from 0x%x to 0x%x" % (ep, dst))

# Miniloader: image at 0x200000, shim copies payload -> fd_base. The shim
# (code + literals) must stay outside the destination while it runs.
load = 0x200000
if load + payload > fd_base and load < fd_base + fd_size:
    fail("miniloader copy would overwrite the shim")
print("check-image: ok: shim 0x%x bytes, booti at 0x%x runs in place, FD 0x%x at 0x%x" % (payload, ep, fd_size, fd_base))

/** @file
  The screen FDT: the device tree that describes the panel, its rails and its
  backlight. RkDisplayDxe brings the panel up from it.

  On the card this is the U-Boot screen DTB, BOOT/rg351mp-uboot.dtb (the same
  file is also stored as rg351v-*, rg351p-* and *-kernel.dtb; mkimg.sh copies
  one file to all six names). It is the DTB U-Boot itself inits the panel
  from, so its delays, rails and init commands are the proven ones. The
  Android DTB that U-Boot passes in x0 also has a panel node, but on several
  consoles it differs (other delays, other supply, other init sequence), so it
  is only a fallback for the chainloaded test.

  Whoever has the file (the boot app, once U-Boot is gone) installs this
  protocol on a new handle. RkDisplayDxe waits for it with a protocol notify
  when it found no usable FDT at its own start.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef RK_SCREEN_FDT_H_
#define RK_SCREEN_FDT_H_

#define RK_SCREEN_FDT_PROTOCOL_GUID \
  { 0x15e48db0, 0xa55a, 0x4e2f, { 0x83, 0xd3, 0x17, 0xbb, 0x5c, 0x9f, 0x91, 0x75 } }

#define RK_SCREEN_FDT_PROTOCOL_REVISION  1

typedef enum {
  RkScreenFdtFromBootPartition = 0,   // BOOT/rg351*-uboot.dtb, loaded by the boot app
  RkScreenFdtFromBootloader    = 1,   // the FDT U-Boot passed in x0 (Android DTB)
  RkScreenFdtFromBootloaderCopy = 2   // a screen DTB the boot.ini hook loaded next to it
} RK_SCREEN_FDT_SOURCE;

typedef struct {
  UINT32                Revision;
  RK_SCREEN_FDT_SOURCE  Source;
  // The blob must stay valid until ExitBootServices (pool or pages, not stack).
  CONST VOID            *Fdt;
  UINTN                 Size;
} RK_SCREEN_FDT_PROTOCOL;

extern EFI_GUID  gRkScreenFdtProtocolGuid;

#endif

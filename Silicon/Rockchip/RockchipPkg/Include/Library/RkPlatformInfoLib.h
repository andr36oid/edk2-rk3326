/** @file
  Facts about the running board that the boot chain left behind: RAM size,
  the scanned-out framebuffer, the FDT the previous stage passed in x0.
  Safe to call from SEC (no memory allocation, MMIO only).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _RK_PLATFORM_INFO_LIB_H_
#define _RK_PLATFORM_INFO_LIB_H_

#include <Uefi.h>

//
// Where U-Boot's boot.ini puts the DTB it passes in x0 (fdt_addr_r). The
// memory map keeps this window, so an FDT is only trusted inside it.
//
#define RK_BOOTLOADER_FDT_BASE  0x01F00000
#define RK_BOOTLOADER_FDT_SIZE  0x00100000

typedef struct {
  EFI_PHYSICAL_ADDRESS  Base;
  UINT32                Width;         // visible pixels
  UINT32                Height;
  UINT32                Stride;        // bytes per line
  UINT32                BitsPerPixel;  // 32, 24 or 16
  UINTN                 VopBase;       // the VOP scanning it out
} RK_FRAMEBUFFER_INFO;

/**
  RAM size in bytes from the DDR init's record (PMUGRF OS_REG2/3).
  Returns 0 when the record is missing or makes no sense.
**/
UINT64
EFIAPI
RkGetDramSize (
  VOID
  );

/**
  The framebuffer a VOP is scanning out right now (left by U-Boot).
**/
EFI_STATUS
EFIAPI
RkGetActiveFramebuffer (
  OUT RK_FRAMEBUFFER_INFO  *Info
  );

//
// What the miniloader recorded about this boot (its parameter tags at
// 2 MB - 8 KB). Boot device bits as in the tags: eMMC BIT1, SD BIT2/BIT3.
//
#define RK_BOOT_DEVICE_EMMC  BIT1
#define RK_BOOT_DEVICE_SD0   BIT2
#define RK_BOOT_DEVICE_SD1   BIT3

typedef struct {
  UINT32                  BootDevType;   // 0 if unknown
  UINT32                  BootDevNum;
  EFI_PHYSICAL_ADDRESS    TeeBase;       // OP-TEE, 0 if unknown
  UINT32                  TeeSize;
} RK_BOOT_TAGS;

/**
  Read the miniloader's tags. Fields it has no (valid) tag for stay 0.
  Returns EFI_NOT_FOUND if there is no tag list at all.
**/
EFI_STATUS
EFIAPI
RkGetBootTags (
  OUT RK_BOOT_TAGS  *Tags
  );

/**
  The FDT the previous boot stage passed in x0, or 0. Only valid from SEC
  (the value lives in the SEC image); DXE reads the copy in the HOB.
**/
EFI_PHYSICAL_ADDRESS
EFIAPI
RkGetBootloaderFdt (
  VOID
  );

#endif

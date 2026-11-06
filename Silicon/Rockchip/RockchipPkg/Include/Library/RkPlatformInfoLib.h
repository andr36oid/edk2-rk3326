/** @file
  Facts about the running board that the boot chain left behind: RAM size,
  the scanned-out framebuffer, the FDT the previous stage passed in x0.
  Safe to call from SEC (no memory allocation, MMIO only).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _RK_PLATFORM_INFO_LIB_H_
#define _RK_PLATFORM_INFO_LIB_H_

#include <Uefi.h>

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

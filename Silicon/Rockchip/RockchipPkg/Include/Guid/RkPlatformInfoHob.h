/** @file
  HOB with what SEC learned about the board (see RkPlatformInfoLib.h).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _RK_PLATFORM_INFO_HOB_H_
#define _RK_PLATFORM_INFO_HOB_H_

#include <Library/RkPlatformInfoLib.h>

#define RK_PLATFORM_INFO_HOB_VERSION  1

typedef struct {
  UINT32                  Version;
  UINT32                  Reserved;
  UINT64                  DramSize;          // 0 if the DDR record was unreadable
  EFI_PHYSICAL_ADDRESS    BootloaderFdt;     // FDT from x0 (U-Boot booti), 0 if none
  UINT32                  BootloaderFdtSize;
  UINT32                  FramebufferValid;
  RK_FRAMEBUFFER_INFO     Framebuffer;
} RK_PLATFORM_INFO_HOB;

extern EFI_GUID  gRkPlatformInfoHobGuid;

#endif

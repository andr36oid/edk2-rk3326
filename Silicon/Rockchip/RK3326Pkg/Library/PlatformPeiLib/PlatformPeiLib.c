/** @file
  RK3326 PEI-phase setup inside SEC: FV HOB plus the board facts HOB.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/RkPlatformInfoLib.h>
#include <Guid/RkPlatformInfoHob.h>

#define FDT_MAGIC  0xD00DFEED

STATIC
UINT32
FdtSize (
  IN EFI_PHYSICAL_ADDRESS  Fdt
  )
{
  CONST UINT32  *Header = (CONST UINT32 *)(UINTN)Fdt;
  UINT32        Size;

  if ((Fdt == 0) || ((Fdt & 7) != 0) || (SwapBytes32 (Header[0]) != FDT_MAGIC)) {
    return 0;
  }

  Size = SwapBytes32 (Header[1]);
  return (Size >= 64 && Size <= SIZE_2MB) ? Size : 0;
}

EFI_STATUS
EFIAPI
PlatformPeim (
  VOID
  )
{
  RK_PLATFORM_INFO_HOB  Info;

  BuildFvHob (PcdGet64 (PcdFvBaseAddress), PcdGet32 (PcdFvSize));

  ZeroMem (&Info, sizeof (Info));
  Info.Version           = RK_PLATFORM_INFO_HOB_VERSION;
  Info.DramSize          = RkGetDramSize ();
  Info.BootloaderFdt     = RkGetBootloaderFdt ();
  Info.BootloaderFdtSize = FdtSize (Info.BootloaderFdt);
  if (Info.BootloaderFdtSize == 0) {
    Info.BootloaderFdt = 0;
  }

  Info.FramebufferValid = !EFI_ERROR (RkGetActiveFramebuffer (&Info.Framebuffer));

  DEBUG ((
    DEBUG_INFO,
    "RK3326: DRAM %lu MB, bootloader FDT 0x%lx (%u bytes), fb 0x%lx %ux%u %ubpp\n",
    Info.DramSize >> 20,
    Info.BootloaderFdt,
    Info.BootloaderFdtSize,
    Info.Framebuffer.Base,
    Info.Framebuffer.Width,
    Info.Framebuffer.Height,
    Info.Framebuffer.BitsPerPixel
    ));

  BuildGuidDataHob (&gRkPlatformInfoHobGuid, &Info, sizeof (Info));
  return EFI_SUCCESS;
}

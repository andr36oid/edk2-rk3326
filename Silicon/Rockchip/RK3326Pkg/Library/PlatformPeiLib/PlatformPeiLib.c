/** @file
  RK3326 PEI-phase setup inside SEC: FV HOB plus the board facts HOB.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/ArmLib.h>
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

  // x0 is only an FDT when U-Boot's booti started us. Started by BL31 (as
  // uboot.img) it is whatever BL31 left there: don't read through it unless
  // it points into the window the memory map protects.
  if ((Fdt < RK_BOOTLOADER_FDT_BASE) || (Fdt >= RK_BOOTLOADER_FDT_BASE + RK_BOOTLOADER_FDT_SIZE) ||
      ((Fdt & 7) != 0) || (SwapBytes32 (Header[0]) != FDT_MAGIC))
  {
    return 0;
  }

  Size = SwapBytes32 (Header[1]);
  if ((Size < 64) || (Size > RK_BOOTLOADER_FDT_BASE + RK_BOOTLOADER_FDT_SIZE - Fdt)) {
    return 0;
  }

  return Size;
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
  if (EFI_ERROR (RkGetBootTags (&Info.BootTags))) {
    DEBUG ((DEBUG_WARN, "RK3326: no miniloader tags at 0x1FE000\n"));
  }

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
  DEBUG ((
    DEBUG_INFO,
    "RK3326: boot device 0x%x/%u, OP-TEE 0x%lx +0x%x\n",
    Info.BootTags.BootDevType,
    Info.BootTags.BootDevNum,
    Info.BootTags.TeeBase,
    Info.BootTags.TeeSize
    ));

  // Linux takes the timer frequency from CNTFRQ (px30.dtsi gives none) and
  // only the firmware at EL3 can set it; the EL tells what BL31 handed over.
  {
    UINT64  El;

    __asm__ volatile ("mrs %0, CurrentEL" : "=r" (El));
    DEBUG ((DEBUG_INFO, "RK3326: running at EL%u, CNTFRQ %u Hz\n", (UINT32)((El >> 2) & 3), (UINT32)ArmReadCntFrq ()));
    if (ArmReadCntFrq () != 24000000) {
      DEBUG ((DEBUG_WARN, "RK3326: CNTFRQ is not 24 MHz, Linux will use it as it is\n"));
    }
  }

  BuildGuidDataHob (&gRkPlatformInfoHobGuid, &Info, sizeof (Info));
  return EFI_SUCCESS;
}

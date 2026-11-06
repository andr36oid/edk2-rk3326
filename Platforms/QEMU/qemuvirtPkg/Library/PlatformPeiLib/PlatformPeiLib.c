/** @file
  QEMU virt PEI-phase setup inside SEC: FV HOB plus the platform info HOB.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Guid/QemuVirtPlatformInfoHob.h>

extern UINT64  gQemuVirtFdtArg;

EFI_STATUS
EFIAPI
PlatformPeim (
  VOID
  )
{
  QEMU_VIRT_PLATFORM_INFO  Info;

  BuildFvHob (PcdGet64 (PcdFvBaseAddress), PcdGet32 (PcdFvSize));

  QemuVirtGetPlatformInfo (&Info);
  DEBUG ((
    DEBUG_INIT,
    "QEMU virt: RAM 0x%lx+%lu MB, FDT 0x%lx (%u bytes, x0 was 0x%lx)\n",
    Info.RamBase,
    Info.RamSize >> 20,
    Info.Fdt,
    Info.FdtSize,
    gQemuVirtFdtArg
    ));
  if (gQemuVirtFdtArg != QEMU_VIRT_FDT_BASE) {
    DEBUG ((DEBUG_WARN, "QEMU virt: x0 is not the FDT window, the memory map may be off\n"));
  }

  BuildGuidDataHob (&gQemuVirtPlatformInfoHobGuid, &Info, sizeof (Info));
  return EFI_SUCCESS;
}

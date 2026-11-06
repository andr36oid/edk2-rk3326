/** @file
  Runs the board init early in DXE, ahead of the SD/eMMC and display drivers.
  When U-Boot ran before us (its FDT is in the platform info HOB) it already
  did the pins and GPIOs, so only the clocks and IO domains are checked then;
  PcdRkBoardInitForce redoes everything for testing under U-Boot.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Guid/RkPlatformInfoHob.h>

#include "BoardInit.h"

EFI_STATUS
EFIAPI
BoardInitDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE           *Hob;
  CONST RK_PLATFORM_INFO_HOB  *Info;
  BOARD_INIT_PLAN             Plan;
  BOOLEAN                     Chained;
  BOOLEAN                     FromEmmc;

  Plan.Force    = FixedPcdGetBool (PcdRkBoardInitForce);
  Plan.Fdt      = NULL;
  Chained       = FALSE;
  FromEmmc      = FALSE;

  Hob = GetFirstGuidHob (&gRkPlatformInfoHobGuid);
  if (Hob != NULL) {
    Info = GET_GUID_HOB_DATA (Hob);
    if ((Info->BootloaderFdt != 0) && (FdtCheckHeader ((VOID *)(UINTN)Info->BootloaderFdt) == 0)) {
      Plan.Fdt = (VOID *)(UINTN)Info->BootloaderFdt;
      Chained  = TRUE;
    }

    if (Info->Version >= 2) {
      FromEmmc = (Info->BootTags.BootDevType & RK_BOOT_DEVICE_EMMC) != 0;
    }
  }

  // The eMMC pins are GPIO1_A0-A7, which are button pins on an SD boot.
  FromEmmc      = FromEmmc || FixedPcdGetBool (PcdRkEmmcAlwaysProbe);
  Plan.Pins     = Plan.Force || !Chained;
  Plan.EmmcPins = Plan.Pins && FromEmmc;

  DEBUG ((DEBUG_INFO, "BoardInit: %a\n", Chained ? "U-Boot ran before, checking only" : "no U-Boot, full init"));
  BoardInitRun (&Plan);

  // Nothing to keep resident or publish.
  return EFI_SUCCESS;
}

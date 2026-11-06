/** @file
  No PMIC access (QEMU, or until a board has a real instance).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseMemoryLib.h>
#include <Library/RkPmicLib.h>

EFI_STATUS
EFIAPI
RkPmicInit (
  VOID
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
RkPmicSetRegulator (
  IN CONST VOID  *Fdt,
  IN INT32       RegulatorNode,
  IN BOOLEAN     Enable
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
RkPmicEnableBootOn (
  IN CONST VOID  *Fdt
  )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
RkPmicGetPowerStatus (
  OUT RK_POWER_STATUS  *Status
  )
{
  if (Status != NULL) {
    ZeroMem (Status, sizeof (*Status));
  }

  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
RkPmicPowerOff (
  VOID
  )
{
  return EFI_UNSUPPORTED;
}

/** @file
  No power checks (QEMU: no battery).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/PowerCheckLib.h>

POWER_CHECK_RESULT
EFIAPI
PowerCheck (
  VOID
  )
{
  return PowerCheckContinue;
}

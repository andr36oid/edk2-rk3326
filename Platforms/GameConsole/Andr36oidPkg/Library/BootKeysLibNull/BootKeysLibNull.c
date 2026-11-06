/** @file
  No buttons (QEMU, host tests).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BootKeysLib.h>

BOARD_FAMILY
EFIAPI
BootKeysGetBoardFamily (
  VOID
  )
{
  return BoardFamilyUnknown;
}

BOOLEAN
EFIAPI
BootKeyPressed (
  IN BOOT_KEY  Key
  )
{
  return FALSE;
}

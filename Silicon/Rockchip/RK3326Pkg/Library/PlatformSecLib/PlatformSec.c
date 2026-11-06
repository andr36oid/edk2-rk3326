/** @file
  RK3326 SEC hooks.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/PlatformSecLib.h>
#include <Library/RkLogLib.h>
#include <Library/RkPlatformInfoLib.h>

// Written by PlatformAssemblyInitialize before anything touches x0.
UINT64  gRkBootloaderFdtArg = 0;

EFI_PHYSICAL_ADDRESS
EFIAPI
RkGetBootloaderFdt (
  VOID
  )
{
  return gRkBootloaderFdtArg;
}

VOID
PlatformInitialize (
  )
{
  // Before the first DEBUG(): keep the last boot's log, start this one.
  RkLogStart ();
}

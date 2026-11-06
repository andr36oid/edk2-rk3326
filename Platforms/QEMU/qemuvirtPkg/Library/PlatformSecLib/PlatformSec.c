/** @file
  QEMU virt SEC hooks.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/PlatformSecLib.h>
#include <Library/RkLogLib.h>

// Written by PlatformAssemblyInitialize before anything touches x0.
UINT64  gQemuVirtFdtArg = 0;

VOID
PlatformInitialize (
  )
{
  // Same as on the RK3326: the log ring starts before the first DEBUG().
  RkLogStart ();
}

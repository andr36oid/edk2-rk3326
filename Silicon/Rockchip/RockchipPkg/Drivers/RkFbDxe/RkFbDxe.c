/** @file
  GOP on the framebuffer the previous stage left on the VOP (U-Boot's boot
  logo screen): size, stride and pixel format come from the VOP registers
  (gRkPlatformInfoHobGuid), so one image drives every panel. The GOP itself
  is RkGopLib; RkDisplayDxe does the same plus panel bring-up.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/RkPlatformInfoHob.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/RkGopLib.h>

EFI_STATUS
EFIAPI
RkFbDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE           *Hob;
  CONST RK_PLATFORM_INFO_HOB  *Info;

  Hob = GetFirstGuidHob (&gRkPlatformInfoHobGuid);
  if (Hob == NULL) {
    return EFI_NOT_FOUND;
  }

  Info = GET_GUID_HOB_DATA (Hob);
  if (!Info->FramebufferValid) {
    DEBUG ((DEBUG_WARN, "RkFbDxe: no active VOP window, no display\n"));
    return EFI_NOT_FOUND;
  }

  return RkGopInstall (&Info->Framebuffer, NULL);
}

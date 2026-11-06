/** @file
  DeviceBootManagerLib for andr36oid devices: SiliciumPkg's BDS calls it
  after registering its own boot options; this puts the andr36oid boot app
  (Application/Andr36oidBoot, in the firmware volume) first in BootOrder.
  Variables are not kept across starts (emulated NV), so this runs every time.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>

extern EFI_GUID  gAndr36oidBootAppFileGuid;

STATIC
VOID
PutBootAppFirst (
  VOID
  )
{
  EFI_STATUS                         Status;
  EFI_LOADED_IMAGE_PROTOCOL          *Image;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  Node;
  EFI_DEVICE_PATH_PROTOCOL           *Path;
  EFI_BOOT_MANAGER_LOAD_OPTION       Option;
  EFI_BOOT_MANAGER_LOAD_OPTION       *Options;
  UINTN                              Count;
  INTN                               Index;

  // The FV BDS came from holds the app too.
  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
  if (EFI_ERROR (Status)) {
    return;
  }

  EfiInitializeFwVolDevicepathNode (&Node, &gAndr36oidBootAppFileGuid);
  Path = AppendDevicePathNode (DevicePathFromHandle (Image->DeviceHandle), (EFI_DEVICE_PATH_PROTOCOL *)&Node);
  if (Path == NULL) {
    return;
  }

  Status = EfiBootManagerInitializeLoadOption (
             &Option,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE,
             L"andr36oid",
             Path,
             NULL,
             0
             );
  FreePool (Path);
  if (EFI_ERROR (Status)) {
    return;
  }

  Options = EfiBootManagerGetLoadOptions (&Count, LoadOptionTypeBoot);
  Index   = EfiBootManagerFindLoadOption (&Option, Options, Count);
  if (Index >= 0) {
    // Already there (a warm start keeps variables): move it to the front.
    EfiBootManagerDeleteLoadOptionVariable (Options[Index].OptionNumber, LoadOptionTypeBoot);
  }

  Status = EfiBootManagerAddLoadOptionVariable (&Option, 0);
  DEBUG ((DEBUG_INIT, "andr36oid: boot app first in BootOrder: %r\n", Status));
  EfiBootManagerFreeLoadOptions (Options, Count);
  EfiBootManagerFreeLoadOption (&Option);
}

//
// The QEMU test device has no screen or keypad: its serial console (the
// SimpleTextInOutSerial handle) becomes ConIn/ConOut.
//
STATIC
VOID
AddSerialConsole (
  VOID
  )
{
  EFI_HANDLE                *Handles;
  UINTN                     Count;
  UINTN                     I;
  VOID                      *In;
  EFI_DEVICE_PATH_PROTOCOL  *Path;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextOutProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  for (I = 0; I < Count; I++) {
    Path = DevicePathFromHandle (Handles[I]);
    if ((Path == NULL) || EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiSimpleTextInProtocolGuid, &In))) {
      continue;
    }

    EfiBootManagerUpdateConsoleVariable (ConOut, Path, NULL);
    EfiBootManagerUpdateConsoleVariable (ErrOut, Path, NULL);
    EfiBootManagerUpdateConsoleVariable (ConIn, Path, NULL);
  }

  FreePool (Handles);
}

//
// The USB host port has no PCI bus for BDS to find it on, so BDS never
// connects it by itself. Connect it here, once the console variables list the
// USB keyboard (SiliciumPkg registers that before calling us), so a keyboard
// plugged in at power on is in ConIn when the boot app starts. The marker
// protocol is on the controller handle of the platform's USB host driver;
// platforms without one find no handle and skip this.
//
STATIC
VOID
ConnectUsbHost (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       I;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gRkUsbHostControllerGuid, NULL, &Count, &Handles))) {
    return;
  }

  for (I = 0; I < Count; I++) {
    DEBUG ((DEBUG_INIT, "andr36oid: USB host: %r\n", gBS->ConnectController (Handles[I], NULL, NULL, TRUE)));
  }

  FreePool (Handles);
}

EFI_HANDLE
EFIAPI
DeviceBootManagerBeforeConsole (
  OUT EFI_DEVICE_PATH_PROTOCOL   **DevicePath,
  OUT BDS_CONSOLE_CONNECT_ENTRY  **PlatformConsoles
  )
{
  if (FeaturePcdGet (PcdBootSerialConsole)) {
    AddSerialConsole ();
  }

  PutBootAppFirst ();
  ConnectUsbHost ();
  return NULL;
}

EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
DeviceBootManagerAfterConsole (
  VOID
  )
{
  return NULL;
}

EFI_DEVICE_PATH_PROTOCOL **
EFIAPI
DeviceBootManagerOnDemandConInConnect (
  VOID
  )
{
  return NULL;
}

VOID
EFIAPI
DeviceBootManagerProcessBootCompletion (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
}

EFI_STATUS
EFIAPI
DeviceBootManagerPriorityBoot (
  EFI_BOOT_MANAGER_LOAD_OPTION  *BootOption
  )
{
  return EFI_NOT_FOUND;
}

VOID
EFIAPI
DeviceBootManagerUnableToBoot (
  VOID
  )
{
}

VOID
EFIAPI
DeviceBootManagerBdsEntry (
  VOID
  )
{
}

/** @file
  The screen when U-Boot did not set it up: RkDisplayDxe waits for the
  panel's DTB (BOOT/rg351*-uboot.dtb, the file U-Boot inits the panel from),
  and a display that comes up that late, or one whose device path BDS does
  not list in ConOut, is put on the console here so the menu shows on it.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Andr36oidBoot.h"

#include <Guid/ConsoleOutDevice.h>
#include <Guid/GlobalVariable.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/RkLogLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/RkScreenFdt.h>

#define SCREEN_DTB_MAX  SIZE_1MB

STATIC RK_SCREEN_FDT_PROTOCOL  mScreenFdt;
STATIC BOOLEAN                 mScreenFdtTried;

//
// RK_SCREEN_FDT_PROTOCOL with the screen DTB U-Boot would load for this
// hwrev. If the display driver is waiting for it, it brings the panel up
// before InstallMultipleProtocolInterfaces returns.
//
STATIC
VOID
GiveScreenFdt (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR8        *HwRev
  )
{
  CONST CHAR8  *Name = BootPolicyScreenDtbName (HwRev);
  VOID         *Fdt;
  UINTN        Size;
  EFI_HANDLE   Handle = NULL;
  EFI_STATUS   Status;

  Status = BootReadFile (Root, Name, SCREEN_DTB_MAX, &Fdt, &Size);
  if (EFI_ERROR (Status)) {
    Say ("boot: screen: no BOOT/%a (%r)\n", Name, Status);
    return;
  }

  if ((Size < 64) || (FdtCheckHeader (Fdt) != 0) || (Fdt32ToCpu (((CONST UINT32 *)Fdt)[1]) > Size)) {
    Say ("boot: screen: BOOT/%a is not a device tree\n", Name);
    FreePool (Fdt);
    return;
  }

  // Pool memory: stays valid until ExitBootServices, as the protocol wants.
  mScreenFdt.Revision = RK_SCREEN_FDT_PROTOCOL_REVISION;
  mScreenFdt.Source   = RkScreenFdtFromBootPartition;
  mScreenFdt.Fdt      = Fdt;
  mScreenFdt.Size     = Size;
  Status              = gBS->InstallMultipleProtocolInterfaces (&Handle, &gRkScreenFdtProtocolGuid, &mScreenFdt, NULL);
  Say ("boot: screen: BOOT/%a (%lu bytes) for the display: %r\n", Name, (UINT64)Size, Status);
  if (EFI_ERROR (Status)) {
    FreePool (Fdt);
  }
}

STATIC
BOOLEAN
InPathList (
  IN EFI_DEVICE_PATH_PROTOCOL  *List OPTIONAL,
  IN EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Walk = List;
  EFI_DEVICE_PATH_PROTOCOL  *One;
  UINTN                     Size;
  UINTN                     PathSize = GetDevicePathSize (Path);
  BOOLEAN                   Found    = FALSE;

  while (!Found && (Walk != NULL)) {
    One = GetNextDevicePathInstance (&Walk, &Size);
    if (One == NULL) {
      break;
    }

    Found = (Size == PathSize) && (CompareMem (One, Path, PathSize - END_DEVICE_PATH_LENGTH) == 0);
    FreePool (One);
  }

  return Found;
}

//
// ConPlatform only puts a display on the console if its device path is in
// ConOut (BDS lists the generic framebuffer's, not RkDisplayDxe's), and a
// display that appeared after BDS connected the consoles is not connected
// at all: add it to ConOut and connect it (GraphicsConsole, ConPlatform,
// ConSplitter). The screen log stops drawing once the console owns it.
//
STATIC
VOID
ConnectDisplays (
  VOID
  )
{
  EFI_HANDLE                *Handles;
  UINTN                     Count;
  UINTN                     I;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  EFI_DEVICE_PATH_PROTOCOL  *ConOut;
  EFI_DEVICE_PATH_PROTOCOL  *New;
  VOID                      *Dummy;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles))) {
    return;
  }

  for (I = 0; I < Count; I++) {
    Path = DevicePathFromHandle (Handles[I]);
    if ((Handles[I] == gST->ConsoleOutHandle) || (Path == NULL) ||
        !EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiConsoleOutDeviceGuid, &Dummy)))
    {
      continue;     // ConSplitter's own, or already on the console
    }

    ConOut = NULL;
    GetEfiGlobalVariable2 (L"ConOut", (VOID **)&ConOut, NULL);
    if (!InPathList (ConOut, Path)) {
      New = AppendDevicePathInstance (ConOut, Path);
      if (New != NULL) {
        gRT->SetVariable (
               L"ConOut",
               &gEfiGlobalVariableGuid,
               EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
               GetDevicePathSize (New),
               New
               );
        FreePool (New);
      }
    }

    if (ConOut != NULL) {
      FreePool (ConOut);
    }

    gBS->ConnectController (Handles[I], NULL, NULL, TRUE);
    if (!EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiConsoleOutDeviceGuid, &Dummy))) {
      RkLogScreen (FALSE);
      Say ("boot: screen: display on the console\n");
    } else {
      Say ("boot: screen: display not on the console, the screen log stays\n");
    }
  }

  FreePool (Handles);
}

VOID
BootScreenSetup (
  IN EFI_FILE_PROTOCOL  *Root OPTIONAL,
  IN CONST CHAR8        *HwRev
  )
{
  if ((Root != NULL) && !mScreenFdtTried) {
    mScreenFdtTried = TRUE;
    GiveScreenFdt (Root, HwRev);
  }

  ConnectDisplays ();
}

BOOLEAN
BootScreenFramebuffer (
  OUT UINT64  *Base,
  OUT UINT64  *Size
  )
{
  EFI_HANDLE                    *Handles;
  UINTN                         Count;
  UINTN                         I;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  BOOLEAN                       Found = FALSE;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles))) {
    return FALSE;
  }

  for (I = 0; !Found && (I < Count); I++) {
    if ((Handles[I] == gST->ConsoleOutHandle) || (DevicePathFromHandle (Handles[I]) == NULL) ||
        EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop)) ||
        (Gop->Mode == NULL) || (Gop->Mode->FrameBufferBase == 0) || (Gop->Mode->FrameBufferSize == 0))
    {
      continue;
    }

    *Base = Gop->Mode->FrameBufferBase;
    *Size = Gop->Mode->FrameBufferSize;
    Found = TRUE;
  }

  FreePool (Handles);
  return Found;
}

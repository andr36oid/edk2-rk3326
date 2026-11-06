/** @file
  Display for the RK3326/PX30 consoles: takes over the screen U-Boot left
  on, or brings the DSI panel up from the screen FDT, and puts a GOP on it.

  Modes (PcdRkDisplayMode, overridden by andr36oid.display= in the
  bootloader's /chosen/bootargs):
    adopt  - U-Boot's screen is on: keep it, GOP on its framebuffer.
    reinit - shut U-Boot's screen down and bring it up again from the FDT
             (the test of the bring-up path while U-Boot still chainloads us).
  A dark screen (no VOP scanning out) is always brought up from the FDT.

  Which FDT, first that works:
    1. andr36oid.screen_fdt=<addr>: the screen DTB the boot.ini hook loaded
       into the reserved FDT window.
    2. The FDT U-Boot passed in x0 (the Android DTB), if its panel parses.
    3. RK_SCREEN_FDT_PROTOCOL, now or whenever someone installs it.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Guid/RkPlatformInfoHob.h>
#include <Protocol/RkScreenFdt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/RkGopLib.h>
#include <Library/RkLogLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "RkDisplay.h"

#define MODE_ADOPT   0
#define MODE_REINIT  1

STATIC RK_DISPLAY                  *mDisplay;
STATIC CONST RK_PLATFORM_INFO_HOB  *mInfo;
STATIC EFI_PHYSICAL_ADDRESS        mBootloaderFdt;
STATIC UINT8                       mMode;
STATIC BOOLEAN                     mGopInstalled;
STATIC BOOLEAN                     mDone;
STATIC RK_FRAMEBUFFER_INFO         mGopFb;
STATIC EFI_EVENT                   mNotifyEvent;
STATIC VOID                        *mNotifyRegistration;

/**
  Value of Key= in the space-separated Args, or NULL.
**/
STATIC
CONST CHAR8 *
BootArg (
  IN  CONST CHAR8  *Args,
  IN  UINTN        ArgsLen,
  IN  CONST CHAR8  *Key,
  OUT UINTN        *ValueLen
  )
{
  UINTN  KeyLen;
  UINTN  Pos;
  UINTN  End;

  KeyLen = AsciiStrLen (Key);
  Pos    = 0;
  while ((Args != NULL) && (Pos < ArgsLen) && (Args[Pos] != '\0')) {
    while ((Pos < ArgsLen) && (Args[Pos] == ' ')) {
      Pos++;
    }

    for (End = Pos; (End < ArgsLen) && (Args[End] != ' ') && (Args[End] != '\0'); End++) {
    }

    if ((End - Pos > KeyLen) && (CompareMem (&Args[Pos], Key, KeyLen) == 0) && (Args[Pos + KeyLen] == '=')) {
      *ValueLen = End - Pos - KeyLen - 1;
      return &Args[Pos + KeyLen + 1];
    }

    Pos = End;
  }

  return NULL;
}

STATIC
BOOLEAN
FdtUsable (
  IN EFI_PHYSICAL_ADDRESS  Address,
  IN UINTN                 Limit
  )
{
  CONST UINT32  *Header;

  if ((Address == 0) || (Limit < 64)) {
    return FALSE;
  }

  Header = (CONST UINT32 *)(UINTN)Address;
  return (FdtCheckHeader (Header) == 0) && (Fdt32ToCpu (Header[1]) <= Limit);
}

/**
  Mode and screen FDT address from the bootloader's bootargs.
**/
STATIC
VOID
ReadBootArgs (
  OUT EFI_PHYSICAL_ADDRESS  *ScreenFdt
  )
{
  CONST VOID   *Fdt;
  INT32        Chosen;
  CONST CHAR8  *Args;
  INT32        ArgsLen;
  CONST CHAR8  *Value;
  UINTN        ValueLen;
  CHAR8        Hex[20];
  UINTN        Address;

  *ScreenFdt = 0;
  if (mBootloaderFdt == 0) {
    return;
  }

  Fdt    = (CONST VOID *)(UINTN)mBootloaderFdt;
  Chosen = FdtPathOffset (Fdt, "/chosen");
  Args   = (Chosen < 0) ? NULL : FdtGetProp (Fdt, Chosen, "bootargs", &ArgsLen);
  if (Args == NULL) {
    return;
  }

  Value = BootArg (Args, (UINTN)ArgsLen, "andr36oid.display", &ValueLen);
  if (Value != NULL) {
    if ((ValueLen == 5) && (CompareMem (Value, "adopt", 5) == 0)) {
      mMode = MODE_ADOPT;
    } else if ((ValueLen == 6) && (CompareMem (Value, "reinit", 6) == 0)) {
      mMode = MODE_REINIT;
    } else {
      DEBUG ((DEBUG_WARN, "Display: andr36oid.display= wants adopt or reinit\n"));
    }
  }

  Value = BootArg (Args, (UINTN)ArgsLen, "andr36oid.screen_fdt", &ValueLen);
  if ((Value == NULL) || (ValueLen == 0) || (ValueLen >= sizeof (Hex))) {
    return;
  }

  CopyMem (Hex, Value, ValueLen);
  Hex[ValueLen] = '\0';
  Address       = AsciiStrHexToUintn (Hex);

  // Only inside the window the memory map keeps for FDTs.
  if ((Address < RK_BOOTLOADER_FDT_BASE) || (Address >= RK_BOOTLOADER_FDT_BASE + RK_BOOTLOADER_FDT_SIZE) ||
      !FdtUsable (Address, RK_BOOTLOADER_FDT_BASE + RK_BOOTLOADER_FDT_SIZE - Address))
  {
    DEBUG ((DEBUG_ERROR, "Display: andr36oid.screen_fdt=%a is not an FDT in the reserved window\n", Hex));
    return;
  }

  *ScreenFdt = Address;
}

STATIC
EFI_STATUS
AllocateFramebuffer (
  IN OUT RK_DISPLAY  *Display
  )
{
  EFI_PHYSICAL_ADDRESS  Base;
  UINTN                 Size;
  EFI_STATUS            Status;

  Display->FbWidth  = Display->Panel.Mode.HDisplay;
  Display->FbHeight = Display->Panel.Mode.VDisplay;
  Display->FbBpp    = 32;
  Display->FbStride = Display->FbWidth * 4;
  Size              = (UINTN)Display->FbStride * Display->FbHeight;

  // The VOP takes a 32-bit address and keeps reading after
  // ExitBootServices, so below 4 GB and reserved.
  Base   = 0xFFFFFFFF;
  Status = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, EFI_SIZE_TO_PAGES (Size), &Base);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Display: no memory for a %ux%u framebuffer\n", Display->FbWidth, Display->FbHeight));
    return Status;
  }

  ZeroMem ((VOID *)(UINTN)Base, Size);
  Display->FbBase = Base;
  DEBUG ((DEBUG_INFO, "Display: new framebuffer 0x%lx, %ux%u, 32 bpp\n", Base, Display->FbWidth, Display->FbHeight));
  return EFI_SUCCESS;
}

/**
  U-Boot's framebuffer can carry on if it is the panel's size: the
  on-screen log keeps drawing into it and an adopted GOP stays valid.
**/
STATIC
BOOLEAN
ReuseFramebuffer (
  IN OUT RK_DISPLAY  *Display
  )
{
  CONST RK_FRAMEBUFFER_INFO  *Fb;

  if ((mInfo == NULL) || !mInfo->FramebufferValid) {
    return FALSE;
  }

  Fb = &mInfo->Framebuffer;
  if ((Fb->Width != Display->Panel.Mode.HDisplay) || (Fb->Height != Display->Panel.Mode.VDisplay) ||
      ((Fb->BitsPerPixel != 32) && (Fb->BitsPerPixel != 24) && (Fb->BitsPerPixel != 16)) ||
      ((Fb->Stride & 3) != 0) || (Fb->Base > 0xFFFFFFFF))
  {
    return FALSE;
  }

  Display->FbBase   = Fb->Base;
  Display->FbWidth  = Fb->Width;
  Display->FbHeight = Fb->Height;
  Display->FbStride = Fb->Stride;
  Display->FbBpp    = Fb->BitsPerPixel;
  DEBUG ((DEBUG_INFO, "Display: keeping U-Boot's framebuffer 0x%lx, %u bpp\n", Fb->Base, Fb->BitsPerPixel));
  return TRUE;
}

STATIC
VOID
InstallGop (
  IN CONST RK_FRAMEBUFFER_INFO  *Fb
  )
{
  // The on-screen log follows the panel (a no-op when it is U-Boot's buffer).
  RkLogSetFramebuffer (Fb->Base, Fb->Width, Fb->Height, Fb->Stride, Fb->BitsPerPixel);
  if (!EFI_ERROR (RkGopInstall (Fb, NULL))) {
    mGopInstalled = TRUE;
    CopyMem (&mGopFb, Fb, sizeof (mGopFb));
  }
}

/**
  Parse Fdt and act on it for the current mode.
**/
STATIC
EFI_STATUS
BringUp (
  IN CONST VOID   *Fdt,
  IN CONST CHAR8  *Source
  )
{
  EFI_STATUS           Status;
  BOOLEAN              ScreenOn;
  BOOLEAN              Reused;
  RK_FRAMEBUFFER_INFO  Fb;

  DEBUG ((DEBUG_INFO, "Display: screen FDT from %a at 0x%p\n", Source, Fdt));
  ZeroMem (mDisplay, sizeof (*mDisplay));
  Status = RkPanelParse (Fdt, &mDisplay->Panel);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Display: no usable panel in that FDT: %r\n", Status));
    return Status;
  }

  RkPanelLog (&mDisplay->Panel);

  ScreenOn = (mInfo != NULL) && mInfo->FramebufferValid;
  if (ScreenOn && (mMode == MODE_ADOPT)) {
    DEBUG ((DEBUG_INFO, "Display: adopt mode, U-Boot's screen stays as it is\n"));
    mDone = TRUE;
    return EFI_SUCCESS;
  }

  if (ScreenOn) {
    if (mInfo->Framebuffer.VopBase != mDisplay->Panel.VopBase) {
      DEBUG ((DEBUG_ERROR, "Display: U-Boot's screen is on VOP 0x%lx, not the FDT's; leaving it\n", (UINT64)mInfo->Framebuffer.VopBase));
      return EFI_UNSUPPORTED;
    }

    Reused = ReuseFramebuffer (mDisplay);
    if (!Reused && mGopInstalled) {
      // The GOP already hands out U-Boot's framebuffer; a new one would
      // leave its users drawing off screen.
      DEBUG ((DEBUG_ERROR, "Display: U-Boot's framebuffer does not fit the panel and the GOP is out; no reinit\n"));
      return EFI_UNSUPPORTED;
    }

    DEBUG ((DEBUG_INFO, "Display: reinit mode\n"));
    RkDisplayShutdown (mDisplay);
  } else {
    Reused = FALSE;
  }

  if (!Reused) {
    Status = AllocateFramebuffer (mDisplay);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = RkDisplayEnable (mDisplay);
  DEBUG ((DEBUG_INFO, "Display: bring-up %a: %r\n", mDisplay->Panel.PanelNodeName, Status));
  mDone = TRUE;

  if (!mGopInstalled) {
    ZeroMem (&Fb, sizeof (Fb));
    Fb.Base         = mDisplay->FbBase;
    Fb.Width        = mDisplay->FbWidth;
    Fb.Height       = mDisplay->FbHeight;
    Fb.Stride       = mDisplay->FbStride;
    Fb.BitsPerPixel = mDisplay->FbBpp;
    Fb.VopBase      = mDisplay->Panel.VopBase;
    InstallGop (&Fb);
  }

  // A panel that failed part of its init may still show something; the
  // GOP stays either way so the console has somewhere to go.
  return Status;
}

STATIC
VOID
EFIAPI
OnScreenFdt (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  RK_SCREEN_FDT_PROTOCOL  *Proto;

  while (!mDone) {
    if (EFI_ERROR (gBS->LocateProtocol (&gRkScreenFdtProtocolGuid, mNotifyRegistration, (VOID **)&Proto))) {
      return;
    }

    if ((Proto->Revision < RK_SCREEN_FDT_PROTOCOL_REVISION) || !FdtUsable ((UINTN)Proto->Fdt, Proto->Size)) {
      DEBUG ((DEBUG_ERROR, "Display: RK_SCREEN_FDT_PROTOCOL carries no valid FDT\n"));
      continue;
    }

    BringUp (Proto->Fdt, "RK_SCREEN_FDT_PROTOCOL");
  }

  gBS->CloseEvent (Event);
}

EFI_STATUS
EFIAPI
RkDisplayDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE     *Hob;
  EFI_PHYSICAL_ADDRESS  ScreenFdt;
  EFI_STATUS            Status;

  Hob   = GetFirstGuidHob (&gRkPlatformInfoHobGuid);
  mInfo = (Hob != NULL) ? GET_GUID_HOB_DATA (Hob) : NULL;
  if ((mInfo != NULL) && FdtUsable (mInfo->BootloaderFdt, mInfo->BootloaderFdtSize)) {
    mBootloaderFdt = mInfo->BootloaderFdt;
  }

  mDisplay = AllocateZeroPool (sizeof (*mDisplay));
  if (mDisplay == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  mMode = FixedPcdGet8 (PcdRkDisplayMode);
  ReadBootArgs (&ScreenFdt);
  DEBUG ((
    DEBUG_INFO,
    "Display: mode %a, U-Boot screen %a, bootloader FDT 0x%lx, screen FDT 0x%lx\n",
    mMode == MODE_REINIT ? "reinit" : "adopt",
    (mInfo != NULL && mInfo->FramebufferValid) ? "on" : "off",
    mBootloaderFdt,
    ScreenFdt
    ));

  // Adopt: the GOP goes up first, whatever the FDT says.
  if ((mInfo != NULL) && mInfo->FramebufferValid && (mMode == MODE_ADOPT)) {
    InstallGop (&mInfo->Framebuffer);
  }

  Status = EFI_NOT_FOUND;
  if (ScreenFdt != 0) {
    Status = BringUp ((CONST VOID *)(UINTN)ScreenFdt, "andr36oid.screen_fdt");
  }

  if (!mDone && (mBootloaderFdt != 0)) {
    Status = BringUp ((CONST VOID *)(UINTN)mBootloaderFdt, "the bootloader (x0)");
  }

  if (mDone) {
    return EFI_SUCCESS;
  }

  // Keep U-Boot's screen usable while waiting for an FDT to reinit from.
  if ((mInfo != NULL) && mInfo->FramebufferValid && !mGopInstalled) {
    InstallGop (&mInfo->Framebuffer);
  }

  DEBUG ((DEBUG_INFO, "Display: waiting for RK_SCREEN_FDT_PROTOCOL\n"));
  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnScreenFdt, NULL, &mNotifyEvent);
  if (!EFI_ERROR (Status)) {
    Status = gBS->RegisterProtocolNotify (&gRkScreenFdtProtocolGuid, mNotifyEvent, &mNotifyRegistration);
  }

  if (!EFI_ERROR (Status)) {
    // Someone may have installed it before us.
    gBS->SignalEvent (mNotifyEvent);
  }

  return EFI_SUCCESS;
}

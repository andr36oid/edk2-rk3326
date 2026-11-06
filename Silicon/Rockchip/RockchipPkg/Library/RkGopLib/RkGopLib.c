/** @file
  GOP on a framebuffer a VOP scans out. Moved here from RkFbDxe so the
  display driver can publish its own framebuffer the same way.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/RkGopLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>

typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} RK_FB_DEVICE_PATH;

STATIC RK_FB_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x5d6f2c0a, 0x8e34, 0x4b7a, { 0x9c, 0x15, 0x2e, 0x60, 0xd8, 0x4b, 0xa3, 0x71 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL          mGop;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE     mMode;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mInfo;
STATIC FRAME_BUFFER_CONFIGURE                *mBltConfig;

STATIC
EFI_STATUS
EFIAPI
GopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  if ((SizeOfInfo == NULL) || (Info == NULL) || (ModeNumber != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *Info = AllocateCopyPool (sizeof (mInfo), &mInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (mInfo);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
GopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  return (ModeNumber == 0) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
GopBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer OPTIONAL,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN UINTN                              SourceX,
  IN UINTN                              SourceY,
  IN UINTN                              DestinationX,
  IN UINTN                              DestinationY,
  IN UINTN                              Width,
  IN UINTN                              Height,
  IN UINTN                              Delta OPTIONAL
  )
{
  return FrameBufferBlt (
           mBltConfig,
           BltBuffer,
           BltOperation,
           SourceX,
           SourceY,
           DestinationX,
           DestinationY,
           Width,
           Height,
           Delta
           );
}

STATIC
EFI_STATUS
FillModeInfo (
  IN CONST RK_FRAMEBUFFER_INFO  *Fb
  )
{
  UINT32  BytesPerPixel = Fb->BitsPerPixel / 8;

  mInfo.Version              = 0;
  mInfo.HorizontalResolution = Fb->Width;
  mInfo.VerticalResolution   = Fb->Height;
  mInfo.PixelsPerScanLine    = Fb->Stride / BytesPerPixel;

  switch (Fb->BitsPerPixel) {
    case 32:
      // VOP ARGB8888 is B, G, R, A in memory.
      mInfo.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
      break;
    case 24:
      mInfo.PixelFormat                   = PixelBitMask;
      mInfo.PixelInformation.RedMask      = 0x00FF0000;
      mInfo.PixelInformation.GreenMask    = 0x0000FF00;
      mInfo.PixelInformation.BlueMask     = 0x000000FF;
      mInfo.PixelInformation.ReservedMask = 0;
      break;
    case 16:
      mInfo.PixelFormat                   = PixelBitMask;
      mInfo.PixelInformation.RedMask      = 0xF800;
      mInfo.PixelInformation.GreenMask    = 0x07E0;
      mInfo.PixelInformation.BlueMask     = 0x001F;
      mInfo.PixelInformation.ReservedMask = 0;
      break;
    default:
      return EFI_UNSUPPORTED;
  }

  mMode.MaxMode         = 1;
  mMode.Mode            = 0;
  mMode.Info            = &mInfo;
  mMode.SizeOfInfo      = sizeof (mInfo);
  mMode.FrameBufferBase = Fb->Base;
  mMode.FrameBufferSize = (UINTN)Fb->Stride * Fb->Height;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
RkGopInstall (
  IN  CONST RK_FRAMEBUFFER_INFO  *Fb,
  OUT EFI_HANDLE                 *Handle OPTIONAL
  )
{
  UINTN       ConfigSize;
  EFI_STATUS  Status;
  EFI_HANDLE  NewHandle;

  if (mGop.Mode != NULL) {
    return EFI_ALREADY_STARTED;
  }

  Status = FillModeInfo (Fb);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ConfigSize = 0;
  Status     = FrameBufferBltConfigure ((VOID *)(UINTN)mMode.FrameBufferBase, &mInfo, NULL, &ConfigSize);
  if (Status != RETURN_BUFFER_TOO_SMALL) {
    return EFI_UNSUPPORTED;
  }

  mBltConfig = AllocatePool (ConfigSize);
  if (mBltConfig == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = FrameBufferBltConfigure ((VOID *)(UINTN)mMode.FrameBufferBase, &mInfo, mBltConfig, &ConfigSize);
  if (EFI_ERROR (Status)) {
    FreePool (mBltConfig);
    return Status;
  }

  mGop.QueryMode = GopQueryMode;
  mGop.SetMode   = GopSetMode;
  mGop.Blt       = GopBlt;
  mGop.Mode      = &mMode;

  DEBUG ((
    DEBUG_INFO,
    "RkGop: %ux%u, %u bpp, stride %u, at 0x%lx (VOP 0x%lx)\n",
    Fb->Width,
    Fb->Height,
    Fb->BitsPerPixel,
    Fb->Stride,
    Fb->Base,
    (UINT64)Fb->VopBase
    ));

  NewHandle = NULL;
  Status    = gBS->InstallMultipleProtocolInterfaces (
                     &NewHandle,
                     &gEfiDevicePathProtocolGuid,
                     &mDevicePath,
                     &gEfiGraphicsOutputProtocolGuid,
                     &mGop,
                     NULL
                     );
  if (EFI_ERROR (Status)) {
    mGop.Mode = NULL;
    return Status;
  }

  if (Handle != NULL) {
    *Handle = NewHandle;
  }

  return EFI_SUCCESS;
}

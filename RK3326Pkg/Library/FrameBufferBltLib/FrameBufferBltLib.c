/** @file
  FrameBufferBltLib - Library to perform blt operations on a frame buffer.

  Copyright (c) 2007 - 2018, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <Uefi/UefiBaseType.h>
#include <Protocol/GraphicsOutput.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/PcdLib.h>               // <-- added for FixedPcdGet32(PcdFrameBufferRotation)
#include <Library/MemoryAllocationLib.h>  // <-- added for AllocatePool/FreePool if needed

struct FRAME_BUFFER_CONFIGURE {
  UINT32                          PixelsPerScanLine;
  UINT32                          BytesPerPixel;
  UINT32                          Width;    // logical width (GOP reported width)
  UINT32                          Height;   // logical height
  UINT8                           *FrameBuffer;
  EFI_GRAPHICS_PIXEL_FORMAT       PixelFormat;
  EFI_PIXEL_BITMASK               PixelMasks;
  INT8                            PixelShl[4]; // R-G-B-Rsvd
  INT8                            PixelShr[4]; // R-G-B-Rsvd
  UINT8                           LineBuffer[0];
};

CONST EFI_PIXEL_BITMASK mRgbPixelMasks = {
  0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000
};

CONST EFI_PIXEL_BITMASK mBgrPixelMasks = {
  0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000
};

/* ---------------------------
   Rotation helpers
   --------------------------- */

typedef enum {
  FbRotNone = 0,
  FbRot90   = 90,
  FbRot180  = 180,
  FbRot270  = 270
} FB_ROTATION;

/**
  Get rotation from PCD (add PcdFrameBufferRotation to your DEC/DSC).
**/
STATIC
FB_ROTATION
GetFbRotation (
  VOID
  )
{
  UINT32 V = FixedPcdGet32 (PcdDisplayRotation);
  switch (V) {
    case 90:  return FbRot90;
    case 180: return FbRot180;
    case 270: return FbRot270;
    default:  return FbRotNone;
  }
}

/**
  Map logical (x,y) to physical framebuffer coordinates (Xphys,Yphys)
  using rotation and the logical/physical sizes.

  - Wlog/Hlog: logical width/height (what Configure->Width/Height holds)
  - PixelsPerScanLine: physical stride (used only to address pixels later)
**/
STATIC
VOID
MapLogicalToPhysical (
  IN  FB_ROTATION Rot,
  IN  UINT32      Wlog,
  IN  UINT32      Hlog,
  IN  INT32       x,
  IN  INT32       y,
  OUT INT32      *Xphys,
  OUT INT32      *Yphys
  )
{
  switch (Rot) {
    case FbRotNone:
      *Xphys = x; *Yphys = y;
      break;
    case FbRot90:
      // 90 deg CW
      *Xphys = (INT32)(Hlog - 1 - y);
      *Yphys = x;
      break;
    case FbRot180:
      *Xphys = (INT32)(Wlog - 1 - x);
      *Yphys = (INT32)(Hlog - 1 - y);
      break;
    case FbRot270:
      // 270 deg CW (90 deg CCW)
      *Xphys = y;
      *Yphys = (INT32)(Wlog - 1 - x);
      break;
    default:
      *Xphys = x; *Yphys = y;
      break;
  }
}

/**
  Return pointer to physical pixel start (byte pointer)
**/
STATIC
UINT8 *
PixelPtrPhys (
  IN UINT8  *FbBase,
  IN UINT32  BytesPerPixel,
  IN UINT32  PixelsPerScanLine,
  IN INT32   Xphys,
  IN INT32   Yphys
  )
{
  return FbBase + (((UINTN)Yphys * PixelsPerScanLine) + (UINTN)Xphys) * BytesPerPixel;
}

/* ---------------------------
   existing functions (unchanged until modifications)
   --------------------------- */

VOID
FrameBufferBltLibConfigurePixelFormat (
  IN CONST EFI_PIXEL_BITMASK    *BitMask,
  OUT UINT32                    *BytesPerPixel,
  OUT INT8                      *PixelShl,
  OUT INT8                      *PixelShr
  )
{
  UINT8   Index;
  UINT32  *Masks;
  UINT32  MergedMasks;

  ASSERT (BytesPerPixel != NULL);

  MergedMasks = 0;
  Masks = (UINT32*) BitMask;
  for (Index = 0; Index < 3; Index++) {
    ASSERT ((MergedMasks & Masks[Index]) == 0);

    PixelShl[Index] = (INT8) HighBitSet32 (Masks[Index]) - 23 + (Index * 8);
    if (PixelShl[Index] < 0) {
      PixelShr[Index] = -PixelShl[Index];
      PixelShl[Index] = 0;
    } else {
      PixelShr[Index] = 0;
    }
    DEBUG ((DEBUG_INFO, "%d: shl:%d shr:%d mask:%x\n", Index,
            PixelShl[Index], PixelShr[Index], Masks[Index]));

    MergedMasks = (UINT32) (MergedMasks | Masks[Index]);
  }
  MergedMasks = (UINT32) (MergedMasks | Masks[3]);

  ASSERT (MergedMasks != 0);
  *BytesPerPixel = (UINT32) ((HighBitSet32 (MergedMasks) + 7) / 8);
  DEBUG ((DEBUG_INFO, "Bytes per pixel: %d\n", *BytesPerPixel));
}

/**
  Create the configuration for a video frame buffer.

  The configuration is returned in the caller provided buffer.

  @param[in] FrameBuffer       Pointer to the start of the frame buffer.
  @param[in] FrameBufferInfo   Describes the frame buffer characteristics.
  @param[in,out] Configure     The created configuration information.
  @param[in,out] ConfigureSize Size of the configuration information.

  @retval RETURN_SUCCESS            The configuration was successful created.
  @retval RETURN_BUFFER_TOO_SMALL   The Configure is to too small. The required
                                    size is returned in ConfigureSize.
  @retval RETURN_UNSUPPORTED        The requested mode is not supported by
                                    this implementaion.

**/
RETURN_STATUS
EFIAPI
FrameBufferBltConfigure (
  IN     VOID                                  *FrameBuffer,
  IN     EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *FrameBufferInfo,
  IN OUT FRAME_BUFFER_CONFIGURE                *Configure,
  IN OUT UINTN                                 *ConfigureSize
  )
{
  CONST EFI_PIXEL_BITMASK                      *BitMask;
  UINT32                                       BytesPerPixel;
  INT8                                         PixelShl[4];
  INT8                                         PixelShr[4];

  if (ConfigureSize == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  switch (FrameBufferInfo->PixelFormat) {
  case PixelRedGreenBlueReserved8BitPerColor:
    BitMask = &mRgbPixelMasks;
    break;

  case PixelBlueGreenRedReserved8BitPerColor:
    BitMask = &mBgrPixelMasks;
    break;

  case PixelBitMask:
    BitMask = &FrameBufferInfo->PixelInformation;
    break;

  case PixelBltOnly:
    ASSERT (FrameBufferInfo->PixelFormat != PixelBltOnly);
    return RETURN_UNSUPPORTED;

  default:
    ASSERT (FALSE);
    return RETURN_INVALID_PARAMETER;
  }

    // Allow PSL < HorizontalResolution when we are rotating (90/270) since
    // logical width != physical stride. Non-rotated still enforces the check.
    UINT32 Rot = FixedPcdGet32 (PcdDisplayRotation);
    if ((Rot == 0 || Rot == 180) &&
        (FrameBufferInfo->PixelsPerScanLine < FrameBufferInfo->HorizontalResolution)) {
      return RETURN_UNSUPPORTED;
    }

  FrameBufferBltLibConfigurePixelFormat (BitMask, &BytesPerPixel, PixelShl, PixelShr);

  if (*ConfigureSize < sizeof (FRAME_BUFFER_CONFIGURE)
                     + FrameBufferInfo->HorizontalResolution * BytesPerPixel) {
    *ConfigureSize = sizeof (FRAME_BUFFER_CONFIGURE)
                   + FrameBufferInfo->HorizontalResolution * BytesPerPixel;
    return RETURN_BUFFER_TOO_SMALL;
  }

  if (Configure == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  CopyMem (&Configure->PixelMasks, BitMask,  sizeof (*BitMask));
  CopyMem (Configure->PixelShl,    PixelShl, sizeof (PixelShl));
  CopyMem (Configure->PixelShr,    PixelShr, sizeof (PixelShr));

   Configure->BytesPerPixel     = BytesPerPixel;

  Configure->PixelFormat       = FrameBufferInfo->PixelFormat;
  Configure->FrameBuffer       = (UINT8*) FrameBuffer;
  Configure->Width             = FrameBufferInfo->HorizontalResolution; // logical width
  Configure->Height            = FrameBufferInfo->VerticalResolution;   // logical height
  Configure->PixelsPerScanLine = FrameBufferInfo->PixelsPerScanLine;    // physical stride

  return RETURN_SUCCESS;
}

/**
  Performs a UEFI Graphics Output Protocol Blt Video Fill.

  @param[in]  Configure     Pointer to a configuration which was successfully
                            created by FrameBufferBltConfigure ().
  @param[in]  Color         Color to fill the region with.
  @param[in]  DestinationX  X location to start fill operation.
  @param[in]  DestinationY  Y location to start fill operation.
  @param[in]  Width         Width (in pixels) to fill.
  @param[in]  Height        Height to fill.

  @retval  RETURN_INVALID_PARAMETER Invalid parameter was passed in.
  @retval  RETURN_SUCCESS           The video was filled successfully.

**/
EFI_STATUS
FrameBufferBltLibVideoFill (
  IN  FRAME_BUFFER_CONFIGURE        *Configure,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Color,
  IN  UINTN                         DestinationX,
  IN  UINTN                         DestinationY,
  IN  UINTN                         Width,
  IN  UINTN                         Height
  )
{
  UINTN                             IndexX;
  UINTN                             IndexY;
  UINT8                             *Destination;
  UINT8                             Uint8;
  UINT32                            Uint32;
  UINT64                            WideFill;
  BOOLEAN                           UseWideFill;
  BOOLEAN                           LineBufferReady;
  UINTN                             Offset;
  UINTN                             WidthInBytes;
  UINTN                             SizeInBytes;

  if (Configure == NULL || Color == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  //
  // Bounds checks are in logical coordinates (Configure->Width/Height are logical)
  //
  if (DestinationY + Height > Configure->Height) {
    DEBUG ((EFI_D_VERBOSE, "VideoFill: Past screen (Y)\n"));
    return RETURN_INVALID_PARAMETER;
  }

  if (DestinationX + Width > Configure->Width) {
    DEBUG ((EFI_D_VERBOSE, "VideoFill: Past screen (X)\n"));
    return RETURN_INVALID_PARAMETER;
  }

  if (Width == 0 || Height == 0) {
    DEBUG ((EFI_D_VERBOSE, "VideoFill: Width or Height is 0\n"));
    return RETURN_INVALID_PARAMETER;
  }

  WidthInBytes = Width * Configure->BytesPerPixel;

  Uint32 = *(UINT32*) Color;
  WideFill =
    (UINT32) (
    (((Uint32 << Configure->PixelShl[0]) >> Configure->PixelShr[0]) &
     Configure->PixelMasks.RedMask) |
     (((Uint32 << Configure->PixelShl[1]) >> Configure->PixelShr[1]) &
      Configure->PixelMasks.GreenMask) |
      (((Uint32 << Configure->PixelShl[2]) >> Configure->PixelShr[2]) &
       Configure->PixelMasks.BlueMask)
      );
  DEBUG ((EFI_D_VERBOSE, "VideoFill: color=0x%x, wide-fill=0x%x\n",
          Uint32, (UINT32)WideFill));

  //
  // If no rotation, keep the original wide/optimized paths.
  //
  if (GetFbRotation() == FbRotNone) {
    // original fast path unchanged
    UseWideFill = TRUE;
    if ((sizeof (WideFill) % Configure->BytesPerPixel) == 0) {
      for (IndexX = Configure->BytesPerPixel; IndexX < sizeof (WideFill); IndexX++) {
        ((UINT8*) &WideFill)[IndexX] = ((UINT8*) &WideFill)[IndexX % Configure->BytesPerPixel];
      }
    } else {
      for (
        IndexX = 1, Uint8 = ((UINT8*) &WideFill)[0];
        IndexX < Configure->BytesPerPixel;
        IndexX++) {
        if (Uint8 != ((UINT8*) &WideFill)[IndexX]) {
          UseWideFill = FALSE;
          break;
        }
      }
      if (UseWideFill) {
        SetMem (&WideFill, sizeof (WideFill), Uint8);
      }
    }

    if (UseWideFill && (DestinationX == 0) && (Width == Configure->PixelsPerScanLine)) {
      DEBUG ((EFI_D_VERBOSE, "VideoFill (wide, one-shot)\n"));
      Offset = DestinationY * Configure->PixelsPerScanLine;
      Offset = Configure->BytesPerPixel * Offset;
      Destination = Configure->FrameBuffer + Offset;
      SizeInBytes = WidthInBytes * Height;
      if (SizeInBytes >= 8) {
        SetMem32 (Destination, SizeInBytes & ~3, (UINT32) WideFill);
        Destination += SizeInBytes & ~3;
        SizeInBytes &= 3;
      }
      if (SizeInBytes > 0) {
        SetMem (Destination, SizeInBytes, (UINT8) (UINTN) WideFill);
      }
    } else {
      LineBufferReady = FALSE;
      for (IndexY = DestinationY; IndexY < (Height + DestinationY); IndexY++) {
        Offset = (IndexY * Configure->PixelsPerScanLine) + DestinationX;
        Offset = Configure->BytesPerPixel * Offset;
        Destination = Configure->FrameBuffer + Offset;

        if (UseWideFill && (((UINTN) Destination & 7) == 0)) {
          DEBUG ((EFI_D_VERBOSE, "VideoFill (wide)\n"));
          SizeInBytes = WidthInBytes;
          if (SizeInBytes >= 8) {
            SetMem64 (Destination, SizeInBytes & ~7, WideFill);
            Destination += SizeInBytes & ~7;
            SizeInBytes &= 7;
          }
          if (SizeInBytes > 0) {
            CopyMem (Destination, &WideFill, SizeInBytes);
          }
        } else {
          DEBUG ((EFI_D_VERBOSE, "VideoFill (not wide)\n"));
          if (!LineBufferReady) {
            CopyMem (Configure->LineBuffer, &WideFill, Configure->BytesPerPixel);
            for (IndexX = 1; IndexX < Width; ) {
              CopyMem (
                (Configure->LineBuffer + (IndexX * Configure->BytesPerPixel)),
                Configure->LineBuffer,
                MIN (IndexX, Width - IndexX) * Configure->BytesPerPixel
              );
              IndexX += MIN (IndexX, Width - IndexX);
            }
            LineBufferReady = TRUE;
          }
          CopyMem (Destination, Configure->LineBuffer, WidthInBytes);
        }
      }
    }

    return RETURN_SUCCESS;
  }

  //
  // Rotation != 0: fallback to safe per-pixel writes using mapping
  //
  {
    FB_ROTATION Rot = GetFbRotation();
    UINT8 *FbBase = Configure->FrameBuffer;
    UINT32 Bpp = Configure->BytesPerPixel;
    UINT32 PSL = Configure->PixelsPerScanLine;
    UINT8 FillBytes[8]; // enough for up to 8 bytes but we use BytesPerPixel
    UINTN b;

    // copy WideFill into byte array (little-endian as memory expects)
    for (b = 0; b < Configure->BytesPerPixel; ++b) {
      FillBytes[b] = ((UINT8*)&WideFill)[b];
    }

    for (IndexY = 0; IndexY < Height; ++IndexY) {
      for (IndexX = 0; IndexX < Width; ++IndexX) {
        INT32 Xp, Yp;
        MapLogicalToPhysical (Rot, Configure->Width, Configure->Height,
                              (INT32)(DestinationX + IndexX),
                              (INT32)(DestinationY + IndexY),
                              &Xp, &Yp);

        UINT8 *D = PixelPtrPhys (FbBase, Bpp, PSL, Xp, Yp);
        for (b = 0; b < Configure->BytesPerPixel; ++b) {
          D[b] = FillBytes[b];
        }
      }
    }
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
FrameBufferBltLibVideoToBltBuffer (
  IN     FRAME_BUFFER_CONFIGURE          *Configure,
     OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL   *BltBuffer,
  IN     UINTN                           SourceX,
  IN     UINTN                           SourceY,
  IN     UINTN                           DestinationX,
  IN     UINTN                           DestinationY,
  IN     UINTN                           Width,
  IN     UINTN                           Height,
  IN     UINTN                           Delta
  )
{
  UINTN                                  DstY;
  UINTN                                  SrcY;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL          *Blt;
  UINT8                                  *Source;
  UINT8                                  *Destination;
  UINTN                                  IndexX;
  UINT32                                 Uint32;
  UINTN                                  Offset;
  UINTN                                  WidthInBytes;

  if (Configure == NULL || BltBuffer == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  if (SourceY + Height > Configure->Height) {
    return RETURN_INVALID_PARAMETER;
  }

  if (SourceX + Width > Configure->Width) {
    return RETURN_INVALID_PARAMETER;
  }

  if (Width == 0 || Height == 0) {
    return RETURN_INVALID_PARAMETER;
  }

  if (Delta == 0) {
    Delta = Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  }

  WidthInBytes = Width * Configure->BytesPerPixel;

  //
  // If no rotation and PixelFormat is BGR fast path, keep original.
  //
  if (GetFbRotation() == FbRotNone &&
      Configure->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {

    for (SrcY = SourceY, DstY = DestinationY;
         DstY < (Height + DestinationY);
         SrcY++, DstY++) {

      Offset = (SrcY * Configure->PixelsPerScanLine) + SourceX;
      Offset = Configure->BytesPerPixel * Offset;
      Source = Configure->FrameBuffer + Offset;

      Destination = (UINT8 *) BltBuffer + (DstY * Delta) + (DestinationX * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL));

      CopyMem (Destination, Source, WidthInBytes);
    }

    return RETURN_SUCCESS;
  }

  //
  // Rotation != 0 or a PixelFormat requiring translation: do per-pixel mapping + conversion
  //
  {
    FB_ROTATION Rot = GetFbRotation();
    UINT8 *FbBase = Configure->FrameBuffer;
    UINT32 Bpp = Configure->BytesPerPixel;
    UINT32 PSL = Configure->PixelsPerScanLine;

    for (SrcY = 0; SrcY < Height; ++SrcY) {
      for (IndexX = 0; IndexX < Width; ++IndexX) {
        // map logical source pixel (SourceX+IndexX, SourceY+SrcY) -> physical
        INT32 Xp, Yp;
        MapLogicalToPhysical (Rot, Configure->Width, Configure->Height,
                              (INT32)(SourceX + IndexX),
                              (INT32)(SourceY + SrcY),
                              &Xp, &Yp);

        UINT8 *SrcPtr = PixelPtrPhys (FbBase, Bpp, PSL, Xp, Yp);
        // Read native framebuffer pixel (masked/aligned)
        Uint32 = 0;
        for (UINTN b = 0; b < Configure->BytesPerPixel; ++b) {
          ((UINT8*)&Uint32)[b] = SrcPtr[b];
        }

        // Translate to EFI_GRAPHICS_OUTPUT_BLT_PIXEL in BltBuffer
        Blt = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *) ((UINT8*)BltBuffer + ((DestinationY + SrcY) * Delta) + ((DestinationX + IndexX) * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)));

        if (Configure->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
          // raw copy already handled above (we won't get here)
          *(UINT32*) Blt = Uint32;
        } else {
          *(UINT32*) Blt =
            (UINT32) (
            (((Uint32 & Configure->PixelMasks.RedMask) >>
              Configure->PixelShl[0]) << Configure->PixelShr[0]) |
              (((Uint32 & Configure->PixelMasks.GreenMask) >>
                Configure->PixelShl[1]) << Configure->PixelShr[1]) |
                (((Uint32 & Configure->PixelMasks.BlueMask) >>
                  Configure->PixelShl[2]) << Configure->PixelShr[2])
              );
        }
      }
    }
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
FrameBufferBltLibBufferToVideo (
  IN  FRAME_BUFFER_CONFIGURE                *Configure,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL         *BltBuffer,
  IN  UINTN                                 SourceX,
  IN  UINTN                                 SourceY,
  IN  UINTN                                 DestinationX,
  IN  UINTN                                 DestinationY,
  IN  UINTN                                 Width,
  IN  UINTN                                 Height,
  IN  UINTN                                 Delta
  )
{
  UINTN                                    DstY;
  UINTN                                    SrcY;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL            *Blt;
  UINT8                                    *Source;
  UINT8                                    *Destination;
  UINTN                                    IndexX;
  UINT32                                   Uint32;
  UINTN                                    Offset;
  UINTN                                    WidthInBytes;

  if (Configure == NULL || BltBuffer == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  if (DestinationY + Height > Configure->Height) {
    return RETURN_INVALID_PARAMETER;
  }

  if (DestinationX + Width > Configure->Width) {
    return RETURN_INVALID_PARAMETER;
  }

  if (Width == 0 || Height == 0) {
    return RETURN_INVALID_PARAMETER;
  }

  if (Delta == 0) {
    Delta = Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  }

  WidthInBytes = Width * Configure->BytesPerPixel;

  //
  // If no rotation and PixelFormat is BGR fast path, keep original (optimized).
  //
  if (GetFbRotation() == FbRotNone &&
      Configure->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {

    for (SrcY = SourceY, DstY = DestinationY;
         SrcY < (Height + SourceY);
         SrcY++, DstY++) {

      Offset = (DstY * Configure->PixelsPerScanLine) + DestinationX;
      Offset = Configure->BytesPerPixel * Offset;
      Destination = Configure->FrameBuffer + Offset;

      Source = (UINT8 *) BltBuffer + (SrcY * Delta);

      CopyMem (Destination, Source, WidthInBytes);
    }

    return RETURN_SUCCESS;
  }

  //
  // Rotation != 0 or PixelFormat requiring translation: do per-pixel conversion + mapping
  //
  {
    FB_ROTATION Rot = GetFbRotation();
    UINT8 *FbBase = Configure->FrameBuffer;
    UINT32 Bpp = Configure->BytesPerPixel;
    UINT32 PSL = Configure->PixelsPerScanLine;

    for (SrcY = 0; SrcY < Height; ++SrcY) {
      for (IndexX = 0; IndexX < Width; ++IndexX) {
        // Read source BltBuffer pixel
        Blt = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *) ((UINT8*)BltBuffer + ((SourceY + SrcY) * Delta) + ((SourceX + IndexX) * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL)));
        Uint32 = *(UINT32*) Blt;

        // Convert to native framebuffer pixel format
        Uint32 = (UINT32) (
            (((Uint32 << Configure->PixelShl[0]) >> Configure->PixelShr[0]) &
             Configure->PixelMasks.RedMask) |
            (((Uint32 << Configure->PixelShl[1]) >> Configure->PixelShr[1]) &
             Configure->PixelMasks.GreenMask) |
            (((Uint32 << Configure->PixelShl[2]) >> Configure->PixelShr[2]) &
             Configure->PixelMasks.BlueMask)
            );

        // Map logical destination pixel to physical coordinates
        INT32 Xp, Yp;
        MapLogicalToPhysical (Rot, Configure->Width, Configure->Height,
                              (INT32)(DestinationX + IndexX),
                              (INT32)(DestinationY + SrcY),
                              &Xp, &Yp);

        UINT8 *DstPtr = PixelPtrPhys (FbBase, Bpp, PSL, Xp, Yp);
        for (UINTN b = 0; b < Configure->BytesPerPixel; ++b) {
          DstPtr[b] = ((UINT8*)&Uint32)[b];
        }
      }
    }
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
FrameBufferBltLibVideoToVideo (
  IN  FRAME_BUFFER_CONFIGURE                *Configure,
  IN  UINTN                                 SourceX,
  IN  UINTN                                 SourceY,
  IN  UINTN                                 DestinationX,
  IN  UINTN                                 DestinationY,
  IN  UINTN                                 Width,
  IN  UINTN                                 Height
  )
{
  UINT8                                     *Source;
  UINT8                                     *Destination;
  UINTN                                     Offset;
  UINTN                                     WidthInBytes;
  INTN                                      LineStride;

  if (Configure == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  //
  // Bounds checks are logical checks here.
  //
  if (SourceY + Height > Configure->Height) {
    return RETURN_INVALID_PARAMETER;
  }

  if (SourceX + Width > Configure->Width) {
    return RETURN_INVALID_PARAMETER;
  }

  if (DestinationY + Height > Configure->Height) {
    return RETURN_INVALID_PARAMETER;
  }

  if (DestinationX + Width > Configure->Width) {
    return RETURN_INVALID_PARAMETER;
  }

  if (Width == 0 || Height == 0) {
    return RETURN_INVALID_PARAMETER;
  }

  WidthInBytes = Width * Configure->BytesPerPixel;

  //
  // If no rotation: keep original memmove-style copy
  //
  if (GetFbRotation() == FbRotNone) {
    Offset = (SourceY * Configure->PixelsPerScanLine) + SourceX;
    Offset = Configure->BytesPerPixel * Offset;
    Source = Configure->FrameBuffer + Offset;

    Offset = (DestinationY * Configure->PixelsPerScanLine) + DestinationX;
    Offset = Configure->BytesPerPixel * Offset;
    Destination = Configure->FrameBuffer + Offset;

    LineStride = Configure->BytesPerPixel * Configure->PixelsPerScanLine;
    if (Destination > Source) {
      //
      // Copy from last line to avoid source is corrupted by copying
      //
      Source += Height * LineStride;
      Destination += Height * LineStride;
      LineStride = -LineStride;
    }

    while (Height-- > 0) {
      CopyMem (Destination, Source, WidthInBytes);

      Source += LineStride;
      Destination += LineStride;
    }

    return RETURN_SUCCESS;
  }

  //
  // Rotation != 0: do a safe copy using a line scratch buffer to avoid overlap issues,
  // mapping src->phys and dst->phys for each pixel.
  //
  {
    FB_ROTATION Rot = GetFbRotation();
    UINT8 *FbBase = Configure->FrameBuffer;
    UINT32 Bpp = Configure->BytesPerPixel;
    UINT32 PSL = Configure->PixelsPerScanLine;
    UINT32 *Line = AllocatePool (Width * sizeof (UINT32));
    if (Line == NULL) {
      return RETURN_OUT_OF_RESOURCES;
    }

    for (UINTN row = 0; row < Height; ++row) {
      // read logical source row into Line[]
      for (UINTN col = 0; col < Width; ++col) {
        INT32 Xs, Ys;
        MapLogicalToPhysical (Rot, Configure->Width, Configure->Height,
                              (INT32)(SourceX + col),
                              (INT32)(SourceY + row),
                              &Xs, &Ys);
        UINT8 *S = PixelPtrPhys (FbBase, Bpp, PSL, Xs, Ys);
        UINT32 v = 0;
        for (UINTN b = 0; b < Bpp; ++b) {
          ((UINT8*)&v)[b] = S[b];
        }
        Line[col] = v;
      }

      // write logical destination row from Line[]
      for (UINTN col = 0; col < Width; ++col) {
        INT32 Xd, Yd;
        MapLogicalToPhysical (Rot, Configure->Width, Configure->Height,
                              (INT32)(DestinationX + col),
                              (INT32)(DestinationY + row),
                              &Xd, &Yd);
        UINT8 *D = PixelPtrPhys (FbBase, Bpp, PSL, Xd, Yd);
        UINT32 v = Line[col];
        for (UINTN b = 0; b < Bpp; ++b) {
          D[b] = ((UINT8*)&v)[b];
        }
      }
    }

    FreePool (Line);
  }

  return RETURN_SUCCESS;
}

/* FrameBufferBlt() dispatcher unchanged: it calls the above functions */
RETURN_STATUS
EFIAPI
FrameBufferBlt (
  IN     FRAME_BUFFER_CONFIGURE                *Configure,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL         *BltBuffer, OPTIONAL
  IN     EFI_GRAPHICS_OUTPUT_BLT_OPERATION     BltOperation,
  IN     UINTN                                 SourceX,
  IN     UINTN                                 SourceY,
  IN     UINTN                                 DestinationX,
  IN     UINTN                                 DestinationY,
  IN     UINTN                                 Width,
  IN     UINTN                                 Height,
  IN     UINTN                                 Delta
  )
{
  if (Configure == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  switch (BltOperation) {
  case EfiBltVideoToBltBuffer:
    return FrameBufferBltLibVideoToBltBuffer (
             Configure,
             BltBuffer,
             SourceX,
             SourceY,
             DestinationX,
             DestinationY,
             Width,
             Height,
             Delta
             );

  case EfiBltVideoToVideo:
    return FrameBufferBltLibVideoToVideo (
             Configure,
             SourceX,
             SourceY,
             DestinationX,
             DestinationY,
             Width,
             Height
             );

  case EfiBltVideoFill:
    return FrameBufferBltLibVideoFill (
             Configure,
             BltBuffer,
             DestinationX,
             DestinationY,
             Width,
             Height
             );

  case EfiBltBufferToVideo:
    return FrameBufferBltLibBufferToVideo (
             Configure,
             BltBuffer,
             SourceX,
             SourceY,
             DestinationX,
             DestinationY,
             Width,
             Height,
             Delta
             );

  default:
    return RETURN_INVALID_PARAMETER;
  }
}

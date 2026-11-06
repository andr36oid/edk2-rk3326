/** @file
  SerialPortLib for RK3326 handhelds: every byte goes into the RAM log ring
  (RkLogLib.h), onto the screen U-Boot left on (any panel size, read from the
  VOP) and, if PcdRkLogUart is set, out of UART2.

  The state lives in the ring header in reserved RAM, not in module globals,
  so SEC and every DXE module write one shared log.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/RkLogLib.h>
#include <Library/RkPlatformInfoLib.h>
#include <Library/SerialPortLib.h>
#include <Soc/Rk3326.h>
#include "Font.h"

#define LOG_MAGIC       SIGNATURE_32 ('R', 'K', 'L', 'G')
#define LOG_HALF        (RK_LOG_SIZE / 2)

typedef struct {
  UINT32    Magic;
  UINT32    HeaderSize;
  UINT64    Written;        // total bytes ever written; ring position = Written % DataSize
  UINT32    DataSize;
  UINT32    FbValid;
  UINT64    FbBase;
  UINT32    FbWidth;
  UINT32    FbHeight;
  UINT32    FbStride;
  UINT32    FbBytesPerPixel;
  UINT32    Column;
  UINT32    Row;
  UINT32    ScreenOff;
  UINT32    HandedOff;      // set right before Linux starts, see RkLogHandoff
} LOG_HEADER;

#define CURRENT   ((LOG_HEADER *)(UINTN)RK_LOG_BASE)
#define PREVIOUS  ((LOG_HEADER *)(UINTN)(RK_LOG_BASE + LOG_HALF))
#define DATA(h)   ((CHAR8 *)(h) + sizeof (LOG_HEADER))

#define GLYPH_W   (FONT_WIDTH + 1)
#define GLYPH_H   (FONT_HEIGHT - 4)

//
// UART2 (DesignWare 16550, 32-bit registers). U-Boot leaves it at 115200 8N1
// on the M1 pins; we only write.
//
#define UART_THR  0x00
#define UART_LSR  0x14
#define UART_LSR_THRE  BIT5

STATIC
VOID
InitHeader (
  IN LOG_HEADER  *H
  )
{
  RK_FRAMEBUFFER_INFO  Fb;

  ZeroMem (H, sizeof (*H));
  H->HeaderSize = sizeof (LOG_HEADER);
  H->DataSize   = LOG_HALF - sizeof (LOG_HEADER);
  if (!EFI_ERROR (RkGetActiveFramebuffer (&Fb)) && ((Fb.BitsPerPixel == 32) || (Fb.BitsPerPixel == 16))) {
    H->FbValid         = 1;
    H->FbBase          = Fb.Base;
    H->FbWidth         = Fb.Width;
    H->FbHeight        = Fb.Height;
    H->FbStride        = Fb.Stride;
    H->FbBytesPerPixel = Fb.BitsPerPixel / 8;
  }

  H->Magic = LOG_MAGIC;
}

STATIC
BOOLEAN
HeaderValid (
  IN CONST LOG_HEADER  *H
  )
{
  return (H->Magic == LOG_MAGIC) && (H->HeaderSize == sizeof (LOG_HEADER)) &&
         (H->DataSize == LOG_HALF - sizeof (LOG_HEADER));
}

VOID
EFIAPI
RkLogStart (
  VOID
  )
{
  // A log that reached Linux is not kept: Linux has reused that RAM since.
  if (HeaderValid (CURRENT) && (CURRENT->HandedOff == 0)) {
    CopyMem (PREVIOUS, CURRENT, LOG_HALF);
  } else {
    ZeroMem (PREVIOUS, sizeof (LOG_HEADER));
  }

  InitHeader (CURRENT);
}

STATIC
VOID
ScreenPutChar (
  IN LOG_HEADER  *H,
  IN CHAR8       C
  )
{
  STATIC CONST UINT64  Glyphs[] = GLYPH_FONT;
  UINT32               Cols;
  UINT32               Rows;
  UINT8                *Origin;
  UINT64               Glyph;
  UINT32               Bits;
  UINT32               X, Y;

  if (!H->FbValid || H->ScreenOff || !FeaturePcdGet (PcdRkLogScreen)) {
    return;
  }

  Cols = H->FbWidth / GLYPH_W;
  Rows = H->FbHeight / GLYPH_H;

  if (C == '\n') {
    H->Column = 0;
    H->Row++;
  } else if (C == '\r') {
    H->Column = 0;
    return;
  } else if ((C < 32) || (C > 126)) {
    return;
  }

  if (H->Row >= Rows) {
    // Full: start again at the top over a cleared screen.
    for (Y = 0; Y < H->FbHeight; Y++) {
      ZeroMem ((VOID *)(UINTN)(H->FbBase + (UINT64)Y * H->FbStride), H->FbWidth * H->FbBytesPerPixel);
    }

    H->Row    = 0;
    H->Column = 0;
  }

  if (C == '\n') {
    return;
  }

  Glyph  = Glyphs[C - 32];
  Origin = (UINT8 *)(UINTN)(H->FbBase + (UINT64)H->Row * GLYPH_H * H->FbStride +
                            (UINT64)H->Column * GLYPH_W * H->FbBytesPerPixel);
  for (Y = 0; Y < GLYPH_H; Y++) {
    Bits = (Y < 6) ? (UINT32)(Glyph >> 32) >> (Y * FONT_WIDTH) : (UINT32)Glyph >> ((Y - 6) * FONT_WIDTH);
    for (X = 0; X < GLYPH_W; X++) {
      BOOLEAN  On  = (X < FONT_WIDTH) && ((Bits >> X) & 1);
      UINT8    *P  = Origin + (UINTN)Y * H->FbStride + (UINTN)X * H->FbBytesPerPixel;

      if (H->FbBytesPerPixel == 4) {
        *(UINT32 *)P = On ? 0xFFFFFFFF : 0xFF000000;
      } else {
        *(UINT16 *)P = On ? 0xFFFF : 0x0000;
      }
    }
  }

  if (++H->Column >= Cols) {
    H->Column = 0;
    H->Row++;
  }
}

RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  // Every module's DebugLib calls this; only the first one sets the log up.
  if (!HeaderValid (CURRENT)) {
    InitHeader (CURRENT);
  }

  return RETURN_SUCCESS;
}

UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  LOG_HEADER  *H = CURRENT;
  UINTN       Index;

  if ((Buffer == NULL) || !HeaderValid (H)) {
    return 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    DATA (H)[H->Written % H->DataSize] = (CHAR8)Buffer[Index];
    H->Written++;
    ScreenPutChar (H, (CHAR8)Buffer[Index]);
    if (FixedPcdGetBool (PcdRkLogUart)) {
      while ((MmioRead32 (RK3326_UART2_BASE + UART_LSR) & UART_LSR_THRE) == 0) {
      }

      MmioWrite32 (RK3326_UART2_BASE + UART_THR, Buffer[Index]);
    }
  }

  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  return 0;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return FALSE;
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  return RETURN_UNSUPPORTED;
}

VOID
EFIAPI
RkLogScreen (
  IN BOOLEAN  Enable
  )
{
  if (HeaderValid (CURRENT)) {
    CURRENT->ScreenOff = Enable ? 0 : 1;
  }
}

VOID
EFIAPI
RkLogHandoff (
  IN BOOLEAN  HandedOff
  )
{
  if (HeaderValid (CURRENT)) {
    CURRENT->HandedOff = HandedOff ? 1 : 0;
  }
}

UINTN
EFIAPI
RkLogSize (
  IN BOOLEAN  Previous
  )
{
  LOG_HEADER  *H = Previous ? PREVIOUS : CURRENT;

  if (!HeaderValid (H)) {
    return 0;
  }

  return (UINTN)MIN (H->Written, (UINT64)H->DataSize);
}

UINTN
EFIAPI
RkLogRead (
  IN  BOOLEAN  Previous,
  OUT CHAR8    *Buffer,
  IN  UINTN    BufferSize
  )
{
  LOG_HEADER  *H    = Previous ? PREVIOUS : CURRENT;
  UINTN       Size  = RkLogSize (Previous);
  UINTN       Start;
  UINTN       First;

  if ((Buffer == NULL) || (Size == 0)) {
    return 0;
  }

  // Keep the newest bytes if the caller's buffer is short.
  if (Size > BufferSize) {
    Size = BufferSize;
  }

  Start = (UINTN)((H->Written - Size) % H->DataSize);
  First = MIN (Size, H->DataSize - Start);
  CopyMem (Buffer, DATA (H) + Start, First);
  CopyMem (Buffer + First, DATA (H), Size - First);
  return Size;
}

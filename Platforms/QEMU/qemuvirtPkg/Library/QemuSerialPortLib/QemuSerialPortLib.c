/** @file
  SerialPortLib + RkLogLib for the QEMU virt test device: output goes to the
  PL011 (qemu -serial) and into a RAM log ring at the same place in RAM as
  on the RK3326, so the boot app's "save the log" path runs unchanged.
  Input comes from the PL011 too (the boot menu reads the keyboard).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/RkLogLib.h>
#include <Library/SerialPortLib.h>
#include <Guid/QemuVirtPlatformInfoHob.h>

#define PL011_BASE     0x09000000
#define PL011_DR       0x00
#define PL011_FR       0x18
#define PL011_FR_RXFE  BIT4
#define PL011_FR_TXFF  BIT5

#define LOG_MAGIC  SIGNATURE_32 ('Q', 'V', 'L', 'G')
#define LOG_HALF   (QEMU_VIRT_LOG_SIZE / 2)

typedef struct {
  UINT32    Magic;
  UINT32    DataSize;
  UINT64    Written;      // total bytes ever written; ring position = Written % DataSize
  UINT32    HandedOff;    // set right before Linux starts, see RkLogHandoff
  UINT32    Reserved;
} LOG_HEADER;

#define CURRENT   ((LOG_HEADER *)(UINTN)QEMU_VIRT_LOG_BASE)
#define PREVIOUS  ((LOG_HEADER *)(UINTN)(QEMU_VIRT_LOG_BASE + LOG_HALF))
#define DATA(h)   ((CHAR8 *)(h) + sizeof (LOG_HEADER))

STATIC
BOOLEAN
HeaderValid (
  IN CONST LOG_HEADER  *H
  )
{
  return (H->Magic == LOG_MAGIC) && (H->DataSize == LOG_HALF - sizeof (LOG_HEADER));
}

VOID
EFIAPI
RkLogStart (
  VOID
  )
{
  if (HeaderValid (CURRENT) && (CURRENT->HandedOff == 0)) {
    CopyMem (PREVIOUS, CURRENT, LOG_HALF);
  } else {
    ZeroMem (PREVIOUS, sizeof (LOG_HEADER));
  }

  ZeroMem (CURRENT, sizeof (LOG_HEADER));
  CURRENT->DataSize = LOG_HALF - sizeof (LOG_HEADER);
  CURRENT->Magic    = LOG_MAGIC;
}

VOID
EFIAPI
RkLogSetFramebuffer (
  IN UINT64  Base,
  IN UINT32  Width,
  IN UINT32  Height,
  IN UINT32  Stride,
  IN UINT32  BitsPerPixel
  )
{
  // No screen log on QEMU: the console is the serial port.
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

VOID
EFIAPI
RkLogScreen (
  IN BOOLEAN  Enable
  )
{
}

UINTN
EFIAPI
RkLogSize (
  IN BOOLEAN  Previous
  )
{
  LOG_HEADER  *H = Previous ? PREVIOUS : CURRENT;

  return HeaderValid (H) ? (UINTN)MIN (H->Written, (UINT64)H->DataSize) : 0;
}

UINTN
EFIAPI
RkLogRead (
  IN  BOOLEAN  Previous,
  OUT CHAR8    *Buffer,
  IN  UINTN    BufferSize
  )
{
  LOG_HEADER  *H   = Previous ? PREVIOUS : CURRENT;
  UINTN       Size = RkLogSize (Previous);
  UINTN       Start;
  UINTN       First;

  if ((Buffer == NULL) || (Size == 0)) {
    return 0;
  }

  if (Size > BufferSize) {
    Size = BufferSize;
  }

  Start = (UINTN)((H->Written - Size) % H->DataSize);
  First = MIN (Size, H->DataSize - Start);
  CopyMem (Buffer, DATA (H) + Start, First);
  CopyMem (Buffer + First, DATA (H), Size - First);
  return Size;
}

RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
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
  UINTN       I;

  for (I = 0; I < NumberOfBytes; I++) {
    if (HeaderValid (H)) {
      DATA (H)[H->Written % H->DataSize] = (CHAR8)Buffer[I];
      H->Written++;
    }

    while ((MmioRead32 (PL011_BASE + PL011_FR) & PL011_FR_TXFF) != 0) {
    }

    MmioWrite32 (PL011_BASE + PL011_DR, Buffer[I]);
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
  UINTN  I;

  for (I = 0; I < NumberOfBytes; I++) {
    while (!SerialPortPoll ()) {
    }

    Buffer[I] = (UINT8)MmioRead32 (PL011_BASE + PL011_DR);
  }

  return NumberOfBytes;
}

BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return (MmioRead32 (PL011_BASE + PL011_FR) & PL011_FR_RXFE) == 0;
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
  *Control = 0;
  return RETURN_SUCCESS;
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

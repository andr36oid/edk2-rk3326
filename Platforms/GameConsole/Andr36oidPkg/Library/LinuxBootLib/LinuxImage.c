/** @file
  arm64 Image header and legacy uImage parsing. No boot services: the host
  tests build this file with gcc.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/LinuxBootLib.h>

STATIC
UINT64
Le64 (
  IN CONST UINT8  *P
  )
{
  UINT64  V = 0;
  INTN    I;

  for (I = 7; I >= 0; I--) {
    V = (V << 8) | P[I];
  }

  return V;
}

STATIC
UINT32
Be32 (
  IN CONST UINT8  *P
  )
{
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) | ((UINT32)P[2] << 8) | P[3];
}

RETURN_STATUS
EFIAPI
LinuxArm64ParseHeader (
  IN  CONST VOID              *Image,
  IN  UINTN                   Size,
  OUT LINUX_ARM64_IMAGE_INFO  *Info
  )
{
  CONST UINT8  *H = Image;

  if ((Image == NULL) || (Info == NULL) || (Size < LINUX_ARM64_HEADER_SIZE)) {
    return RETURN_INVALID_PARAMETER;
  }

  if ((UINT32)Le64 (H + 56) != LINUX_ARM64_MAGIC) {
    return RETURN_UNSUPPORTED;
  }

  Info->TextOffset = Le64 (H + 8);
  Info->ImageSize  = Le64 (H + 16);
  Info->Flags      = Le64 (H + 24);

  // Before 3.17 image_size was 0 and text_offset was not to be trusted.
  if (Info->ImageSize == 0) {
    Info->TextOffset = LINUX_ARM64_DEFAULT_OFFSET;
    Info->ImageSize  = Size;
  }

  // The kernel clears its BSS, but the file itself must fit.
  if (Info->ImageSize < Size) {
    Info->ImageSize = Size;
  }

  // text_offset is below 2 MB (it is added to a 2 MB aligned base), and
  // big-endian kernels (flags bit 0) are not something we can start.
  if ((Info->TextOffset >= SIZE_2MB) || ((Info->TextOffset & 0xFFF) != 0) || ((Info->Flags & 1) != 0)) {
    return RETURN_UNSUPPORTED;
  }

  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
LinuxUImagePayload (
  IN  CONST VOID  *Buffer,
  IN  UINTN       Size,
  OUT UINTN       *PayloadOffset,
  OUT UINTN       *PayloadSize
  )
{
  CONST UINT8  *H = Buffer;
  UINT32       DataSize;

  if ((Buffer == NULL) || (PayloadOffset == NULL) || (PayloadSize == NULL)) {
    return RETURN_INVALID_PARAMETER;
  }

  if ((Size < LINUX_UIMAGE_HEADER_SIZE) || (Be32 (H) != LINUX_UIMAGE_MAGIC)) {
    return RETURN_UNSUPPORTED;
  }

  //
  // Header (all big-endian): magic, hcrc, time, size at 12, load, ep, dcrc,
  // then os at 28, arch 29, type 30, comp 31. booti takes a ramdisk (type 3)
  // without compression; anything else would not start either way.
  //
  DataSize = Be32 (H + 12);
  if ((H[30] != 3) || (H[31] != 0) || (DataSize == 0) ||
      (DataSize > Size - LINUX_UIMAGE_HEADER_SIZE))
  {
    return RETURN_VOLUME_CORRUPTED;
  }

  *PayloadOffset = LINUX_UIMAGE_HEADER_SIZE;
  *PayloadSize   = DataSize;
  return RETURN_SUCCESS;
}

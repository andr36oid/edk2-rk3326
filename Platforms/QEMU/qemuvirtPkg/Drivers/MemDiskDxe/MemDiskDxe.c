/** @file
  Block device over a RAM region: the BOOT FAT image tools/qemu-test.sh
  loads into the guest (-device loader). qemu has no SD controller the
  firmware drives, so this stands in for the card's BOOT partition. With the
  guest RAM in a shared file the script reads back what the firmware wrote.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define BLOCK_SIZE  512

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} MEM_DISK_DEVICE_PATH;

STATIC MEM_DISK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x93f1a6c5, 0x2e48, 0x4b7d, { 0x8a, 0x0c, 0x51, 0xe9, 0x3d, 0x76, 0xb2, 0x4f } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC EFI_BLOCK_IO_MEDIA     mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo;

STATIC
EFI_STATUS
Check (
  IN UINT32   MediaId,
  IN EFI_LBA  Lba,
  IN UINTN    BufferSize,
  IN VOID     *Buffer
  )
{
  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((Buffer == NULL) || ((BufferSize % BLOCK_SIZE) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Lba > mMedia.LastBlock) || (BufferSize / BLOCK_SIZE > mMedia.LastBlock + 1 - Lba)) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MemDiskReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MemDiskRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  EFI_STATUS  Status = Check (MediaId, Lba, BufferSize, Buffer);

  if (!EFI_ERROR (Status)) {
    CopyMem (Buffer, (VOID *)(UINTN)(FixedPcdGet64 (PcdMemDiskBase) + Lba * BLOCK_SIZE), BufferSize);
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MemDiskWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  EFI_STATUS  Status = Check (MediaId, Lba, BufferSize, Buffer);

  if (!EFI_ERROR (Status)) {
    CopyMem ((VOID *)(UINTN)(FixedPcdGet64 (PcdMemDiskBase) + Lba * BLOCK_SIZE), Buffer, BufferSize);
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MemDiskFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MemDiskDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle = NULL;

  mMedia.MediaId          = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = FALSE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = BLOCK_SIZE;
  mMedia.IoAlign          = 0;
  mMedia.LastBlock        = FixedPcdGet64 (PcdMemDiskSize) / BLOCK_SIZE - 1;

  mBlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION;
  mBlockIo.Media       = &mMedia;
  mBlockIo.Reset       = MemDiskReset;
  mBlockIo.ReadBlocks  = MemDiskRead;
  mBlockIo.WriteBlocks = MemDiskWrite;
  mBlockIo.FlushBlocks = MemDiskFlush;

  DEBUG ((DEBUG_INIT, "MemDisk: BOOT image at 0x%lx, %lu MB\n", FixedPcdGet64 (PcdMemDiskBase), FixedPcdGet64 (PcdMemDiskSize) >> 20));
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEfiBlockIoProtocolGuid,
                &mBlockIo,
                &gEfiDevicePathProtocolGuid,
                &mDevicePath,
                NULL
                );
}

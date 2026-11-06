/** @file
  Block devices over RAM regions: the BOOT FAT images tools/qemu-test.sh
  loads into the guest (-device loader). qemu has no SD/eMMC controller the
  firmware drives, so these stand in for the SD card's BOOT partition (at
  PcdMemDiskBase), the eMMC's (right after it) and a USB stick's (after
  that; its device path has a USB() node), the last two only if an image is
  there.
  Like RkSdmmcDxe, the eMMC shows up at start only when the bootloader's
  bootargs say andr36oid.bootpart=0:..., else when gRkEmmcRequestEventGuid
  is signalled. With the guest RAM in a shared file the script reads back
  what the firmware wrote.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/QemuVirtPlatformInfoHob.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define BLOCK_SIZE  512

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} MEM_DISK_DEVICE_PATH;

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  USB_DEVICE_PATH             Usb;
  EFI_DEVICE_PATH_PROTOCOL    End;
} MEM_DISK_USB_DEVICE_PATH;

typedef struct {
  CONST CHAR8                 *Name;
  EFI_PHYSICAL_ADDRESS        Base;
  VOID                        *DevicePath;
  EFI_BLOCK_IO_MEDIA          Media;
  EFI_BLOCK_IO_PROTOCOL       BlockIo;
  EFI_HANDLE                  Handle;
} MEM_DISK;

#define END_NODE  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
#define VENDOR(...)  { { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } }, __VA_ARGS__ }

// gQemuVirtMemDiskGuid: the SD card
STATIC MEM_DISK_DEVICE_PATH  mSdPath = {
  VENDOR ({ 0x93f1a6c5, 0x2e48, 0x4b7d, { 0x8a, 0x0c, 0x51, 0xe9, 0x3d, 0x76, 0xb2, 0x4f } }), END_NODE
};
// gQemuVirtMemDiskEmmcGuid: the eMMC
STATIC MEM_DISK_DEVICE_PATH  mEmmcPath = {
  VENDOR ({ 0x6a3c1f80, 0x4d2e, 0x4b19, { 0x9f, 0x57, 0x0e, 0x84, 0xb3, 0x2a, 0x61, 0xd5 } }), END_NODE
};
// A USB stick on port 0 of a made-up host controller
STATIC MEM_DISK_USB_DEVICE_PATH  mUsbPath = {
  VENDOR ({ 0x2f9e0b6d, 0x71c4, 0x4e8a, { 0xb3, 0x5d, 0x0c, 0x6a, 0x94, 0x1e, 0x27, 0x8f } }),
  { { MESSAGING_DEVICE_PATH, MSG_USB_DP, { sizeof (USB_DEVICE_PATH), 0 } }, 0, 0 },
  END_NODE
};

STATIC MEM_DISK  mDisks[3] = {
  { "SD",   0, &mSdPath   },
  { "eMMC", 0, &mEmmcPath },
  { "USB",  0, &mUsbPath  }
};

STATIC
EFI_STATUS
Check (
  IN MEM_DISK  *Disk,
  IN UINT32    MediaId,
  IN EFI_LBA   Lba,
  IN UINTN     BufferSize,
  IN VOID      *Buffer
  )
{
  if (MediaId != Disk->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((Buffer == NULL) || ((BufferSize % BLOCK_SIZE) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Lba > Disk->Media.LastBlock) || (BufferSize / BLOCK_SIZE > Disk->Media.LastBlock + 1 - Lba)) {
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
  MEM_DISK    *Disk  = BASE_CR (This, MEM_DISK, BlockIo);
  EFI_STATUS  Status = Check (Disk, MediaId, Lba, BufferSize, Buffer);

  if (!EFI_ERROR (Status)) {
    CopyMem (Buffer, (VOID *)(UINTN)(Disk->Base + Lba * BLOCK_SIZE), BufferSize);
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
  MEM_DISK    *Disk  = BASE_CR (This, MEM_DISK, BlockIo);
  EFI_STATUS  Status = Check (Disk, MediaId, Lba, BufferSize, Buffer);

  if (!EFI_ERROR (Status)) {
    CopyMem ((VOID *)(UINTN)(Disk->Base + Lba * BLOCK_SIZE), Buffer, BufferSize);
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

STATIC
EFI_STATUS
AddDisk (
  IN MEM_DISK  *Disk
  )
{
  Disk->Media.MediaId     = 1;
  Disk->Media.MediaPresent = TRUE;
  Disk->Media.BlockSize   = BLOCK_SIZE;
  Disk->Media.LastBlock   = FixedPcdGet64 (PcdMemDiskSize) / BLOCK_SIZE - 1;

  Disk->BlockIo.Revision    = EFI_BLOCK_IO_PROTOCOL_REVISION;
  Disk->BlockIo.Media       = &Disk->Media;
  Disk->BlockIo.Reset       = MemDiskReset;
  Disk->BlockIo.ReadBlocks  = MemDiskRead;
  Disk->BlockIo.WriteBlocks = MemDiskWrite;
  Disk->BlockIo.FlushBlocks = MemDiskFlush;

  DEBUG ((DEBUG_INIT, "MemDisk: %a BOOT image at 0x%lx, %lu MB\n", Disk->Name, Disk->Base, FixedPcdGet64 (PcdMemDiskSize) >> 20));
  return gBS->InstallMultipleProtocolInterfaces (
                &Disk->Handle,
                &gEfiBlockIoProtocolGuid,
                &Disk->BlockIo,
                &gEfiDevicePathProtocolGuid,
                Disk->DevicePath,
                NULL
                );
}

STATIC
VOID
EFIAPI
OnEmmcRequest (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->CloseEvent (Event);
  if (mDisks[1].Handle == NULL) {
    DEBUG ((DEBUG_INIT, "MemDisk: eMMC asked for\n"));
    AddDisk (&mDisks[1]);
  }
}

/**
  The bootloader said the system is on the eMMC (andr36oid.bootpart=0:...).
**/
STATIC
BOOLEAN
BootpartIsEmmc (
  VOID
  )
{
  EFI_HOB_GUID_TYPE        *Hob = GetFirstGuidHob (&gQemuVirtPlatformInfoHobGuid);
  QEMU_VIRT_PLATFORM_INFO  *Qemu;
  CONST CHAR8              *Args;
  INT32                    Node;
  INT32                    Len;

  if (Hob == NULL) {
    return FALSE;
  }

  Qemu = GET_GUID_HOB_DATA (Hob);
  if ((Qemu->Fdt == 0) || (FdtCheckHeader ((VOID *)(UINTN)Qemu->Fdt) != 0)) {
    return FALSE;
  }

  Node = FdtPathOffset ((VOID *)(UINTN)Qemu->Fdt, "/chosen");
  Args = (Node >= 0) ? FdtGetProp ((VOID *)(UINTN)Qemu->Fdt, Node, "bootargs", &Len) : NULL;
  return (Args != NULL) && (Len > 0) && (AsciiStrStr (Args, "andr36oid.bootpart=0:") != NULL);
}

STATIC
BOOLEAN
Loaded (
  IN CONST MEM_DISK  *Disk
  )
{
  CONST UINT8  *Sector = (CONST UINT8 *)(UINTN)Disk->Base;

  return (Sector[510] == 0x55) && (Sector[511] == 0xAA);
}

EFI_STATUS
EFIAPI
MemDiskDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT    Event;
  UINTN        I;

  for (I = 0; I < ARRAY_SIZE (mDisks); I++) {
    mDisks[I].Base = FixedPcdGet64 (PcdMemDiskBase) + I * FixedPcdGet64 (PcdMemDiskSize);
  }

  // No image loaded there: no boot sector signature, no such disk.
  if (Loaded (&mDisks[2])) {
    AddDisk (&mDisks[2]);
  }

  if (Loaded (&mDisks[1])) {
    if (BootpartIsEmmc ()) {
      AddDisk (&mDisks[1]);
    } else {
      gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnEmmcRequest, NULL, &gRkEmmcRequestEventGuid, &Event);
    }
  }

  return AddDisk (&mDisks[0]);
}

/** @file
  BOOT volume access: find it, read and write files, save the log.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Andr36oidBoot.h"

#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/RkLogLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC
EFI_STATUS
OpenFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR8        *Name,
  IN  UINT64             Mode,
  OUT EFI_FILE_PROTOCOL  **File
  )
{
  CHAR16  Name16[64];

  if (EFI_ERROR (AsciiStrToUnicodeStrS (Name, Name16, ARRAY_SIZE (Name16)))) {
    return EFI_INVALID_PARAMETER;
  }

  return Root->Open (Root, File, Name16, Mode, 0);
}

BOOLEAN
BootFileExists (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR8        *Name
  )
{
  EFI_FILE_PROTOCOL  *File;

  if (EFI_ERROR (OpenFile (Root, Name, EFI_FILE_MODE_READ, &File))) {
    return FALSE;
  }

  File->Close (File);
  return TRUE;
}

//
// BDS starts the app from the firmware volume without connecting the storage
// drivers (SD/eMMC, partitions, FAT): connect everything once, as BDS's
// "connect all" does, until no new handles appear.
//
STATIC
VOID
ConnectAll (
  VOID
  )
{
  STATIC BOOLEAN  Done = FALSE;
  EFI_HANDLE      *Handles;
  UINTN           Count;
  UINTN           Before = 0;
  UINTN           Pass;
  UINTN           I;

  if (Done) {
    return;
  }

  Done = TRUE;
  for (Pass = 0; Pass < 4; Pass++) {
    if (EFI_ERROR (gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles))) {
      return;
    }

    if (Count == Before) {
      FreePool (Handles);
      return;
    }

    Before = Count;
    for (I = 0; I < Count; I++) {
      gBS->ConnectController (Handles[I], NULL, NULL, TRUE);
    }

    FreePool (Handles);
  }
}

EFI_FILE_PROTOCOL *
BootOpenVolume (
  IN BOOT_DEVICE_KIND  Kind
  )
{
  EFI_STATUS                       Status;
  EFI_HANDLE                       *Handles;
  UINTN                            Count;
  UINTN                            Pass;
  UINTN                            I;
  BOOT_DEVICE_KIND                 Got;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;

  ConnectAll ();
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  // First the volumes that are on that storage, then (SD only) the ones
  // the platform can't place.
  for (Pass = 0; Pass < 2; Pass++) {
    for (I = 0; I < Count; I++) {
      Got = BootPlatformDeviceKind (DevicePathFromHandle (Handles[I]));
      if (!((Pass == 0) && (Got == Kind)) && !((Pass == 1) && (Kind == BootDeviceSd) && (Got == BootDeviceUnknown))) {
        continue;
      }

      if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) ||
          EFI_ERROR (Fs->OpenVolume (Fs, &Root)))
      {
        continue;
      }

      if (BootFileExists (Root, BOOT_FILE_KERNEL) || BootFileExists (Root, BOOT_FILE_BOOT_INI)) {
        FreePool (Handles);
        return Root;
      }

      Root->Close (Root);
    }
  }

  FreePool (Handles);
  return NULL;
}

EFI_STATUS
BootReadFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR8        *Name,
  IN  UINTN              MaxSize,
  OUT VOID               **Buffer,
  OUT UINTN              *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINT64             FileSize;
  UINTN              Read;
  UINT8              *Data;

  *Buffer = NULL;
  *Size   = 0;
  Status  = OpenFile (Root, Name, EFI_FILE_MODE_READ, &File);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  // The end of the file is its size.
  Status = File->SetPosition (File, MAX_UINT64);
  if (!EFI_ERROR (Status)) {
    Status = File->GetPosition (File, &FileSize);
  }

  if (!EFI_ERROR (Status)) {
    Status = File->SetPosition (File, 0);
  }

  if (EFI_ERROR (Status)) {
    File->Close (File);
    return Status;
  }

  if (FileSize > MaxSize) {
    File->Close (File);
    return EFI_BAD_BUFFER_SIZE;
  }

  Data = AllocatePool ((UINTN)FileSize + 1);
  if (Data == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }

  Read   = (UINTN)FileSize;
  Status = File->Read (File, &Read, Data);
  File->Close (File);
  if (!EFI_ERROR (Status) && (Read != FileSize)) {
    Status = EFI_DEVICE_ERROR;
  }

  if (EFI_ERROR (Status)) {
    FreePool (Data);
    return Status;
  }

  Data[Read] = 0;
  *Buffer    = Data;
  *Size      = Read;
  return EFI_SUCCESS;
}

EFI_STATUS
BootWriteFile (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR8        *Name,
  IN CONST VOID         *Data,
  IN UINTN              Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINTN              Written;

  // Delete first, so a shorter write leaves no old tail.
  if (!EFI_ERROR (OpenFile (Root, Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, &File))) {
    File->Delete (File);
  }

  Status = OpenFile (Root, Name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, &File);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Written = Size;
  Status  = File->Write (File, &Written, (VOID *)Data);
  if (!EFI_ERROR (Status) && (Written != Size)) {
    Status = EFI_VOLUME_FULL;
  }

  if (!EFI_ERROR (Status)) {
    Status = File->Flush (File);
  }

  File->Close (File);
  return Status;
}

STATIC
EFI_STATUS
SaveLog (
  IN EFI_FILE_PROTOCOL  *Root,
  IN BOOLEAN            Previous,
  IN CONST CHAR8        *Name
  )
{
  EFI_STATUS  Status;
  UINTN       Size = RkLogSize (Previous);
  CHAR8       *Buf;

  if (Size == 0) {
    return EFI_NOT_FOUND;
  }

  Buf = AllocatePool (Size);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Size   = RkLogRead (Previous, Buf, Size);
  Status = BootWriteFile (Root, Name, Buf, Size);
  FreePool (Buf);
  return Status;
}

EFI_STATUS
BootSaveLogs (
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  EFI_STATUS  Status;

  if (Root == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = SaveLog (Root, FALSE, BOOT_FILE_LOG);
  DEBUG ((DEBUG_INIT, "boot: log saved to BOOT/%a: %r\n", BOOT_FILE_LOG, Status));
  // No previous log (first start after power-up) leaves an older file alone.
  SaveLog (Root, TRUE, BOOT_FILE_LOG_PREV);
  return Status;
}

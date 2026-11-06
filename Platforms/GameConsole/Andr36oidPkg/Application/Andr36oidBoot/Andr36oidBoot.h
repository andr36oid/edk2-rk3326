/** @file
  andr36oid boot app: does what BOOT/boot.ini does under U-Boot and starts
  Android's kernel directly (LinuxBootLib).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _ANDR36OID_BOOT_H_
#define _ANDR36OID_BOOT_H_

#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BootPlatformLib.h>
#include <Library/BootPolicyLib.h>

#define BOOT_FILE_LOG       "uefi-log.txt"
#define BOOT_FILE_LOG_PREV  "uefi-log-prev.txt"

#define BOOT_TEXT_MAX  SIZE_64KB

//
// Print on the console and into the RAM log (saved as uefi-log.txt).
//
VOID
EFIAPI
Say (
  IN CONST CHAR8  *Format,
  ...
  );

// BootFiles.c

/**
  Root of the first FAT volume on that storage that has Image or boot.ini.
  On a device whose storage can't be told apart (Unknown), any volume with
  them counts as the SD card. NULL if none.
**/
EFI_FILE_PROTOCOL *
BootOpenVolume (
  IN BOOT_DEVICE_KIND  Kind
  );

/**
  Ask the platform for the eMMC on a start that did not come from it
  (gRkEmmcRequestEventGuid: RkSdmmcDxe switches its pins over and adds it);
  the next BootOpenVolume connects it.
**/
VOID
BootRequestEmmc (
  VOID
  );

/**
  Whole file into pool memory, with a 0 after it. EFI_NOT_FOUND if it is
  not there, EFI_BAD_BUFFER_SIZE if larger than MaxSize.
**/
EFI_STATUS
BootReadFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR8        *Name,
  IN  UINTN              MaxSize,
  OUT VOID               **Buffer,
  OUT UINTN              *Size
  );

BOOLEAN
BootFileExists (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR8        *Name
  );

/**
  Replace the file with Data (created if missing) and flush it.
**/
EFI_STATUS
BootWriteFile (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR8        *Name,
  IN CONST VOID         *Data,
  IN UINTN              Size
  );

/**
  The UEFI log (this start and the one before) to uefi-log.txt and
  uefi-log-prev.txt on BOOT.
**/
EFI_STATUS
BootSaveLogs (
  IN EFI_FILE_PROTOCOL  *Root
  );

// BootMenu.c

/**
  The boot menu on the console: d-pad/A through BootKeysLib, arrows/Enter or
  the item's number on a keyboard. Returns what to start; Save log and Power
  off are handled inside. Root may be NULL (no BOOT found).
**/
BOOT_CHOICE
BootMenu (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR8        *Message OPTIONAL
  );

/**
  Power off: the PMIC (RkPmicLib), else ResetSystem. Does not return.
**/
VOID
BootPowerOff (
  VOID
  );

#endif

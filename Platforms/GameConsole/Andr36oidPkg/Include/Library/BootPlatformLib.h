/** @file
  What the boot app needs to know about the machine it runs on, so it builds
  for the RK3326 consoles (r36sPkg: gRkPlatformInfoHobGuid) and for the QEMU
  test device (qemuvirtPkg) without knowing either.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _BOOT_PLATFORM_LIB_H_
#define _BOOT_PLATFORM_LIB_H_

#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Library/LinuxBootLib.h>

typedef enum {
  BootDeviceUnknown,
  BootDeviceSd,           // the card slot (U-Boot's mmc 1)
  BootDeviceEmmc          // built-in eMMC (U-Boot's mmc 0)
} BOOT_DEVICE_KIND;

typedef struct {
  CONST CHAR8             *Name;              // for the log
  BOOT_DEVICE_KIND        BootDevice;         // what the boot ROM/miniloader started from
  UINT64                  DramSize;           // bytes, 0 = unknown
  EFI_PHYSICAL_ADDRESS    BootloaderFdt;      // FDT the previous stage passed in x0, 0 = none
  UINT32                  BootloaderFdtSize;
  BOOLEAN                 FbValid;            // a framebuffer is being scanned out
  EFI_PHYSICAL_ADDRESS    FbBase;
  UINT64                  FbSize;
  //
  // RAM Linux may use when no bootloader FDT says (U-Boot writes /memory
  // there): DRAM minus what firmware below Linux keeps (BL31, OP-TEE).
  //
  UINTN                   MemoryCount;
  LINUX_MEMORY_RANGE      Memory[LINUX_MAX_MEMORY_RANGES];
} BOOT_PLATFORM_INFO;

EFI_STATUS
EFIAPI
BootPlatformGetInfo (
  OUT BOOT_PLATFORM_INFO  *Info
  );

/**
  Which storage a block device / file system handle's device path is on.
**/
BOOT_DEVICE_KIND
EFIAPI
BootPlatformDeviceKind (
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  );

#endif

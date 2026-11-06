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
  BootDeviceEmmc,         // built-in eMMC (U-Boot's mmc 0)
  BootDeviceUsb           // a USB stick (the app tells it from the path, BootPolicyIsUsbPath)
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
  The SoC's unique ID, 16 bytes (BootPolicySerialFromCpuId
  makes U-Boot's serial number from it). Only called when U-Boot gave none.
**/
EFI_STATUS
EFIAPI
BootPlatformGetCpuId (
  OUT UINT8  *CpuId
  );

/**
  Which storage a block device / file system handle's device path is on.
**/
BOOT_DEVICE_KIND
EFIAPI
BootPlatformDeviceKind (
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  );

/**
  A start without U-Boot (no bootloader FDT): switch the USB host port's 5 V
  the way the board DTB from BOOT says, as U-Boot's board code does
  otherwise, then connect the USB host again so what is plugged in shows up.
  Call once.

  @retval EFI_SUCCESS      Host power on (or nothing to switch).
  @retval EFI_NOT_READY    A charger is in on a board that shares the port
                           with it: left charging, no USB devices.
  @retval EFI_UNSUPPORTED  No USB host on this device.
**/
EFI_STATUS
EFIAPI
BootPlatformUsbPower (
  IN CONST VOID  *BoardFdt
  );

#endif

/** @file
  BootPlatformLib for the QEMU virt test device.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BootPlatformLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HobLib.h>
#include <Guid/QemuVirtPlatformInfoHob.h>

extern EFI_GUID  gQemuVirtMemDiskGuid;
extern EFI_GUID  gQemuVirtMemDiskEmmcGuid;

EFI_STATUS
EFIAPI
BootPlatformGetInfo (
  OUT BOOT_PLATFORM_INFO  *Info
  )
{
  EFI_HOB_GUID_TYPE        *Hob;
  QEMU_VIRT_PLATFORM_INFO  *Qemu;

  ZeroMem (Info, sizeof (*Info));
  Info->Name       = "QEMU virt";
  Info->BootDevice = BootDeviceSd;

  Hob = GetFirstGuidHob (&gQemuVirtPlatformInfoHobGuid);
  if (Hob == NULL) {
    return EFI_NOT_FOUND;
  }

  Qemu                    = GET_GUID_HOB_DATA (Hob);
  Info->DramSize          = Qemu->RamSize;
  Info->BootloaderFdt     = Qemu->Fdt;
  Info->BootloaderFdtSize = Qemu->FdtSize;
  // No firmware below us keeps RAM here: Linux may have all of it.
  Info->MemoryCount    = 1;
  Info->Memory[0].Base = Qemu->RamBase;
  Info->Memory[0].Size = Qemu->RamSize;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
BootPlatformGetCpuId (
  OUT UINT8  *CpuId
  )
{
  // QEMU has no OTP: a fixed ID, so the test sees the serial number path.
  CopyMem (CpuId, "QEMU virt cpu id", 16);
  return EFI_SUCCESS;
}

BOOT_DEVICE_KIND
EFIAPI
BootPlatformDeviceKind (
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL  *Node;

  // MemDiskDxe's BOOT images stand in for the SD card and the eMMC.
  for (Node = DevicePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) == HARDWARE_DEVICE_PATH) && (DevicePathSubType (Node) == HW_VENDOR_DP)) {
      if (CompareGuid (&((CONST VENDOR_DEVICE_PATH *)Node)->Guid, &gQemuVirtMemDiskGuid)) {
        return BootDeviceSd;
      }

      if (CompareGuid (&((CONST VENDOR_DEVICE_PATH *)Node)->Guid, &gQemuVirtMemDiskEmmcGuid)) {
        return BootDeviceEmmc;
      }
    }
  }

  return BootDeviceUnknown;
}

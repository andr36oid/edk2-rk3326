/** @file
  BootPlatformLib for the RK3326 consoles: board facts from
  gRkPlatformInfoHobGuid, storage told apart by DW-MMC controller address.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/RkPlatformInfoHob.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BootPlatformLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HobLib.h>
#include <Protocol/NonDiscoverableDevice.h>

#define SDMMC_BASE   0xFF370000
#define EMMC_BASE    0xFF390000

//
// Same layout as MemoryMapLib: BL31 and its shared memory sit in the first
// 2 MB (U-Boot's bidram keeps 0-2 MB out of /memory, ramoops lives there),
// OP-TEE where the miniloader's tags say, else at 0x08400000. Only used when
// no bootloader FDT gives /memory.
//
#define LINUX_RAM_START  0x00200000
#define OPTEE_BASE       0x08400000
#define OPTEE_SIZE       0x01000000

EFI_STATUS
EFIAPI
BootPlatformGetInfo (
  OUT BOOT_PLATFORM_INFO  *Info
  )
{
  EFI_HOB_GUID_TYPE     *Hob;
  RK_PLATFORM_INFO_HOB  *Rk;
  UINT64                TeeBase = OPTEE_BASE;
  UINT64                TeeEnd  = OPTEE_BASE + OPTEE_SIZE;

  ZeroMem (Info, sizeof (*Info));
  Info->Name = "RK3326";

  Hob = GetFirstGuidHob (&gRkPlatformInfoHobGuid);
  if (Hob == NULL) {
    return EFI_NOT_FOUND;
  }

  // Later versions only append fields.
  Rk = GET_GUID_HOB_DATA (Hob);
  if (Rk->Version < 1) {
    return EFI_UNSUPPORTED;
  }

  Info->DramSize          = Rk->DramSize;
  if ((Rk->Version >= 2) && (GET_GUID_HOB_DATA_SIZE (Hob) >= sizeof (*Rk))) {
    if ((Rk->BootTags.BootDevType & RK_BOOT_DEVICE_EMMC) != 0) {
      Info->BootDevice = BootDeviceEmmc;
    } else if ((Rk->BootTags.BootDevType & (RK_BOOT_DEVICE_SD0 | RK_BOOT_DEVICE_SD1)) != 0) {
      Info->BootDevice = BootDeviceSd;
    }
  }

  Info->BootloaderFdt     = Rk->BootloaderFdt;
  Info->BootloaderFdtSize = Rk->BootloaderFdtSize;
  if (Rk->FramebufferValid) {
    Info->FbValid = TRUE;
    Info->FbBase  = Rk->Framebuffer.Base;
    Info->FbSize  = (UINT64)Rk->Framebuffer.Stride * Rk->Framebuffer.Height;
  }

  if ((Rk->Version >= 2) && (GET_GUID_HOB_DATA_SIZE (Hob) >= sizeof (*Rk)) &&
      (Rk->BootTags.TeeSize != 0) && (Rk->BootTags.TeeBase > LINUX_RAM_START) &&
      (Rk->BootTags.TeeBase + Rk->BootTags.TeeSize < Rk->DramSize))
  {
    TeeBase = Rk->BootTags.TeeBase;
    TeeEnd  = Rk->BootTags.TeeBase + Rk->BootTags.TeeSize;
  }

  if (Rk->DramSize > TeeEnd) {
    Info->MemoryCount    = 2;
    Info->Memory[0].Base = LINUX_RAM_START;
    Info->Memory[0].Size = TeeBase - LINUX_RAM_START;
    Info->Memory[1].Base = TeeEnd;
    Info->Memory[1].Size = Rk->DramSize - TeeEnd;
  }

  return EFI_SUCCESS;
}

BOOT_DEVICE_KIND
EFIAPI
BootPlatformDeviceKind (
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL  *Node;
  UINT64                          Base;

  //
  // RkSdmmcDxe's node: VenHw(NonDiscoverableDevice) followed by the
  // controller's base address (UINT64) and resource type.
  //
  for (Node = DevicePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    if ((DevicePathType (Node) != HARDWARE_DEVICE_PATH) || (DevicePathSubType (Node) != HW_VENDOR_DP) ||
        (DevicePathNodeLength (Node) < sizeof (VENDOR_DEVICE_PATH) + sizeof (UINT64)) ||
        !CompareGuid (&((CONST VENDOR_DEVICE_PATH *)Node)->Guid, &gEdkiiNonDiscoverableDeviceProtocolGuid))
    {
      continue;
    }

    CopyMem (&Base, (CONST UINT8 *)Node + sizeof (VENDOR_DEVICE_PATH), sizeof (Base));
    if (Base == SDMMC_BASE) {
      return BootDeviceSd;
    }

    if (Base == EMMC_BASE) {
      return BootDeviceEmmc;
    }
  }

  return BootDeviceUnknown;
}

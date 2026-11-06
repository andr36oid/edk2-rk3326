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
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Soc/Rk3326.h>

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

//
// PX30 OTP (non-secure, px30.dtsi otp@ff290000), read the way the kernel's
// drivers/nvmem/rockchip-otp.c px30_otp_read does. The CPU ID is the otp_id
// cell, 16 bytes at 0x07 (U-Boot's CPUID_OFF/CPUID_LEN, the kernel's
// rockchip-cpuinfo "id").
//
#define OTP_BASE                  0xFF290000
#define OTPC_SBPI_CTRL            0x0020
#define OTPC_SBPI_CMD_VALID_PRE   0x0024
#define OTPC_USER_CTRL            0x0100
#define OTPC_USER_ADDR            0x0104
#define OTPC_USER_ENABLE          0x0108
#define OTPC_USER_Q               0x0124
#define OTPC_INT_STATUS           0x0304
#define OTPC_SBPI_CMD0            0x1000
#define OTPC_SBPI_CMD1            0x1004
#define OTPC_SBPI_DONE            BIT1
#define OTPC_USER_DONE            BIT2
#define OTP_CPU_ID_OFFSET         0x07
#define OTP_TIMEOUT_US            10000

// CRU (clk-px30.c): clk_otp CLKGATE_CON12 bit 11, clk_otp_usr CON13 bit 6,
// pclk_otp_ns CON15 bit 6, pclk_otp_phy CON16 bit 2; SRST_OTP_PHY = 180
// (SOFTRST_CON11 bit 4). Hiword-mask registers, a 0 bit ungates.
#define CRU_CLKGATE(n)   (RK3326_CRU_BASE + 0x200 + 4 * (n))
#define CRU_SOFTRST(n)   (RK3326_CRU_BASE + 0x300 + 4 * (n))
#define HIWORD(bit, on)  ((1U << ((bit) + 16)) | ((on) ? (1U << (bit)) : 0))

STATIC
BOOLEAN
OtpWait (
  IN UINT32  Flag
  )
{
  UINTN  Us;

  for (Us = 0; Us < OTP_TIMEOUT_US; Us++) {
    if ((MmioRead32 (OTP_BASE + OTPC_INT_STATUS) & Flag) != 0) {
      MmioWrite32 (OTP_BASE + OTPC_INT_STATUS, Flag);
      return TRUE;
    }

    MicroSecondDelay (1);
  }

  return FALSE;
}

EFI_STATUS
EFIAPI
BootPlatformGetCpuId (
  OUT UINT8  *CpuId
  )
{
  UINTN  I;

  MmioWrite32 (CRU_CLKGATE (12), HIWORD (11, FALSE));
  MmioWrite32 (CRU_CLKGATE (13), HIWORD (6, FALSE));
  MmioWrite32 (CRU_CLKGATE (15), HIWORD (6, FALSE));
  MmioWrite32 (CRU_CLKGATE (16), HIWORD (2, FALSE));
  MmioWrite32 (CRU_SOFTRST (11), HIWORD (4, TRUE));
  MicroSecondDelay (2);
  MmioWrite32 (CRU_SOFTRST (11), HIWORD (4, FALSE));

  // ECC off: SBPI write of register 0x3A on DAP address 2
  MmioWrite32 (OTP_BASE + OTPC_SBPI_CTRL, 0xFF000000 | (0x02 << 8));
  MmioWrite32 (OTP_BASE + OTPC_SBPI_CMD_VALID_PRE, 0xFFFF0000 | 0x1);
  MmioWrite32 (OTP_BASE + OTPC_SBPI_CMD0, 0xC0 | 0x3A);
  MmioWrite32 (OTP_BASE + OTPC_SBPI_CMD1, 0x09);
  MmioWrite32 (OTP_BASE + OTPC_SBPI_CTRL, HIWORD (0, TRUE));
  if (!OtpWait (OTPC_SBPI_DONE)) {
    return EFI_TIMEOUT;
  }

  MmioWrite32 (OTP_BASE + OTPC_USER_CTRL, HIWORD (0, TRUE));
  MicroSecondDelay (5);
  for (I = 0; I < 16; I++) {
    MmioWrite32 (OTP_BASE + OTPC_USER_ADDR, 0xFFFF0000 | (OTP_CPU_ID_OFFSET + I));
    MmioWrite32 (OTP_BASE + OTPC_USER_ENABLE, HIWORD (0, TRUE));
    if (!OtpWait (OTPC_USER_DONE)) {
      break;
    }

    CpuId[I] = MmioRead8 (OTP_BASE + OTPC_USER_Q);
  }

  MmioWrite32 (OTP_BASE + OTPC_USER_CTRL, HIWORD (0, FALSE));
  return (I == 16) ? EFI_SUCCESS : EFI_TIMEOUT;
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

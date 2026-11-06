/** @file
 *
 *  Synopsys DesignWare MSHC platform driver for Rockchip SDMMC
 *
 *  Copyright (c) 2023, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  andr36oid: RK3326/PX30 clocks and resets, a second controller for the
 *  eMMC some consoles have, and card presence that can't hide the card we
 *  booted from.
 *
 **/

#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CruLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/RkSdmmcPlatformLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Guid/RkPlatformInfoHob.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Protocol/PlatformDwMmc.h>

#define DW_MMC_SIZE  SIZE_16KB

// Card clock the slots run at: SD high speed / eMMC HS52 at 3.3 V signalling.
// No UHS or HS200: those need I/O voltage switching and tuning.
#define DW_MMC_CLOCK_HZ  50000000U

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  UINT64                      BaseAddress;
  UINT8                       ResourceType;
  EFI_DEVICE_PATH_PROTOCOL    End;
} NON_DISCOVERABLE_DEVICE_PATH;
#pragma pack ()

typedef struct {
  CONST CHAR8                          *Name;
  UINT64                               Base;
  UINT8                                ClockIndex;   // CruSetSdmmcClockRate
  UINT8                                ResetCon;     // CRU_SOFTRST_CON / bit of the AHB reset
  UINT8                                ResetBit;
  NON_DISCOVERABLE_DEVICE_PATH         DevicePath;
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR    Resources[1];
  EFI_ACPI_END_TAG_DESCRIPTOR          End;
  NON_DISCOVERABLE_DEVICE              Device;
  DW_MMC_HC_SLOT_CAP                   Capability;
} RK_DW_MMC_SLOT;

STATIC RK_DW_MMC_SLOT  mSlots[] = {
  // SDMMC: SRST_SDMMC_H = 86 (CON5 bit 6)
  { "SD", 0xFF370000, 0, 5, 6 },
  // EMMC:  SRST_EMMC_H = 82 (CON5 bit 2)
  { "eMMC", 0xFF390000, 1, 5, 2 },
};

#define SLOT_SD    0
#define SLOT_EMMC  1

STATIC BOOLEAN  mBootedFromSd;

STATIC
RK_DW_MMC_SLOT *
FindSlot (
  IN EFI_HANDLE  Controller
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mSlots); Index++) {
    if ((mSlots[Index].Capability.Controller != NULL) && (mSlots[Index].Capability.Controller == Controller)) {
      return &mSlots[Index];
    }
  }

  return NULL;
}

STATIC
EFI_STATUS
EFIAPI
RkSdmmcGetCapability (
  IN     EFI_HANDLE          Controller,
  IN     UINT8               Slot,
  OUT    DW_MMC_HC_SLOT_CAP  *Capability
  )
{
  RK_DW_MMC_SLOT  *S = FindSlot (Controller);

  if ((S == NULL) || (Capability == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Capability, &S->Capability, sizeof (DW_MMC_HC_SLOT_CAP));
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
EFIAPI
RkSdmmcCardDetect (
  IN EFI_HANDLE  Controller,
  IN UINT8       Slot
  )
{
  RK_DW_MMC_SLOT               *S = FindSlot (Controller);
  RKSDMMC_CARD_PRESENCE_STATE  PresenceState;

  if (S == NULL) {
    return FALSE;
  }

  // eMMC is soldered on; it is only registered when it is there.
  if (S == &mSlots[SLOT_EMMC]) {
    return TRUE;
  }

  // The card we were loaded from is there, whatever an unwired or odd
  // card-detect pin says.
  if (mBootedFromSd || FixedPcdGetBool (PcdRkSdmmcCardDetectBroken)) {
    return TRUE;
  }

  PresenceState = RkSdmmcGetCardPresenceState ();
  return (PresenceState == RkSdmmcCardPresenceUnsupported) || (PresenceState == RkSdmmcCardPresent);
}

STATIC PLATFORM_DW_MMC_PROTOCOL  mDwMmcDeviceProtocol = {
  RkSdmmcGetCapability,
  RkSdmmcCardDetect
};

/**
  Where this boot came from: the miniloader's boot device tag, else (while
  chainloaded) the andr36oid.bootpart=<dev>:<part> U-Boot put on /chosen/bootargs.
**/
STATIC
VOID
GetBootSource (
  OUT BOOLEAN  *FromSd,
  OUT BOOLEAN  *FromEmmc
  )
{
  EFI_HOB_GUID_TYPE           *Hob;
  CONST RK_PLATFORM_INFO_HOB  *Info;
  CONST CHAR8                 *Args;
  INT32                       Node;
  INT32                       Len;

  *FromSd   = FALSE;
  *FromEmmc = FALSE;

  Hob = GetFirstGuidHob (&gRkPlatformInfoHobGuid);
  if (Hob == NULL) {
    return;
  }

  Info = GET_GUID_HOB_DATA (Hob);
  if (Info->Version >= 2) {
    *FromSd   = (Info->BootTags.BootDevType & (RK_BOOT_DEVICE_SD0 | RK_BOOT_DEVICE_SD1)) != 0;
    *FromEmmc = (Info->BootTags.BootDevType & RK_BOOT_DEVICE_EMMC) != 0;
  }

  if ((Info->BootloaderFdt == 0) || (FdtCheckHeader ((VOID *)(UINTN)Info->BootloaderFdt) != 0)) {
    return;
  }

  Node = FdtPathOffset ((VOID *)(UINTN)Info->BootloaderFdt, "/chosen");
  Args = (Node >= 0) ? FdtGetProp ((VOID *)(UINTN)Info->BootloaderFdt, Node, "bootargs", &Len) : NULL;
  if ((Args == NULL) || (Len <= 0)) {
    return;
  }

  // The BOOT partition U-Boot used says which card the system is on.
  if (AsciiStrStr (Args, "andr36oid.bootpart=0:") != NULL) {
    *FromEmmc = TRUE;
  } else if (AsciiStrStr (Args, "andr36oid.bootpart=1:") != NULL) {
    *FromSd = TRUE;
  }
}

STATIC
EFI_STATUS
AddSlot (
  IN RK_DW_MMC_SLOT  *S
  )
{
  NON_DISCOVERABLE_DEVICE_PATH  *Dp = &S->DevicePath;
  UINTN                         Rate;

  Dp->Vendor.Header.Type    = HARDWARE_DEVICE_PATH;
  Dp->Vendor.Header.SubType = HW_VENDOR_DP;
  SetDevicePathNodeLength (&Dp->Vendor.Header, OFFSET_OF (NON_DISCOVERABLE_DEVICE_PATH, End));
  CopyGuid (&Dp->Vendor.Guid, &gEdkiiNonDiscoverableDeviceProtocolGuid);
  Dp->BaseAddress  = S->Base;
  Dp->ResourceType = ACPI_ADDRESS_SPACE_TYPE_MEM;
  SetDevicePathEndNode (&Dp->End);

  S->Resources[0].Desc                  = ACPI_ADDRESS_SPACE_DESCRIPTOR;
  S->Resources[0].Len                   = sizeof (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR) - 3;
  S->Resources[0].ResType               = ACPI_ADDRESS_SPACE_TYPE_MEM;
  S->Resources[0].AddrSpaceGranularity  = 32;
  S->Resources[0].AddrRangeMin          = S->Base;
  S->Resources[0].AddrRangeMax          = S->Base + DW_MMC_SIZE - 1;
  S->Resources[0].AddrLen               = DW_MMC_SIZE;
  S->End.Desc                           = ACPI_END_TAG_DESCRIPTOR;

  S->Device.Type       = &gDwMmcHcNonDiscoverableDeviceGuid;
  S->Device.DmaType    = NonDiscoverableDeviceDmaTypeNonCoherent;
  S->Device.Initialize = NULL;
  S->Device.Resources  = S->Resources;

  // Clock first, then pulse the controller's AHB reset so it starts clean.
  CruSetSdmmcClockRate (S->ClockIndex, DW_MMC_CLOCK_HZ);
  CruAssertSoftReset (S->ResetCon, S->ResetBit);
  MicroSecondDelay (5);
  CruDeassertSoftReset (S->ResetCon, S->ResetBit);

  Rate = CruGetSdmmcClockRate (S->ClockIndex);
  if (Rate == 0) {
    DEBUG ((DEBUG_ERROR, "RkSdmmc: %a clock unknown, not registered\n", S->Name));
    return EFI_DEVICE_ERROR;
  }

  S->Capability.HighSpeed   = 1;
  S->Capability.Voltage33   = 1;
  S->Capability.BaseClkFreq = (UINT32)(Rate / 1000);
  if (S->ClockIndex == 0) {
    S->Capability.BusWidth = 4;
    S->Capability.SlotType = RemovableSlot;
    S->Capability.CardType = SdCardType;
  } else {
    S->Capability.BusWidth = 8;
    S->Capability.SlotType = EmbeddedSlot;
    S->Capability.CardType = EmmcCardType;
  }

  DEBUG ((DEBUG_INFO, "RkSdmmc: %a at 0x%lx, %u kHz\n", S->Name, S->Base, S->Capability.BaseClkFreq));

  return gBS->InstallMultipleProtocolInterfaces (
                &S->Capability.Controller,
                &gEfiDevicePathProtocolGuid,
                &S->DevicePath,
                &gEdkiiNonDiscoverableDeviceProtocolGuid,
                &S->Device,
                NULL
                );
}

EFI_STATUS
EFIAPI
RkSdmmcDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  BOOLEAN     FromEmmc;

  GetBootSource (&mBootedFromSd, &FromEmmc);
  DEBUG ((DEBUG_INFO, "RkSdmmc: booted from %a\n", mBootedFromSd ? "SD" : (FromEmmc ? "eMMC" : "unknown")));

  // The SD pins: the eMMC ones are left as the boot stage before set them
  // (it read the eMMC), since on consoles without eMMC they may be used
  // for something else.
  RkSdmmcSetIoMux ();
  Status = AddSlot (&mSlots[SLOT_SD]);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RkSdmmc: SD: %r\n", Status));
  }

  if (FromEmmc || FixedPcdGetBool (PcdRkEmmcAlwaysProbe)) {
    Status = AddSlot (&mSlots[SLOT_EMMC]);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "RkSdmmc: eMMC: %r\n", Status));
    }
  }

  Handle = NULL;
  return gBS->InstallProtocolInterface (
                &Handle,
                &gPlatformDwMmcProtocolGuid,
                EFI_NATIVE_INTERFACE,
                &mDwMmcDeviceProtocol
                );
}

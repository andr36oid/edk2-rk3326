/** @file
  This file implement the MMC Host Protocol for the DesignWare eMMC.

  Copyright (c) 2022, Jared McNeill <jmcneill@invisible.ca>
  Copyright (c) 2014-2017, Linaro Limited. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/CruLib.h>
#include <Library/GpioLib.h>

#include <Protocol/MmcHost.h>

#include "Mshc.h"

#define DW_DBG    DEBUG_INFO

#define DWEMMC_DESC_PAGE                1
#define DWEMMC_BLOCK_SIZE               512
#define DWEMMC_DMA_BUF_SIZE             (512 * 8)
#define DWEMMC_MAX_DESC_PAGES           512

STATIC CONST GPIO_IOMUX_CONFIG mSdmmc0IomuxConfig[] = {
  { "sdmmc0_clk",         1, GPIO_PIN_PD6, 1, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "sdmmc0_cmd",         1, GPIO_PIN_PD7, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
  { "sdmmc0_det",         0, GPIO_PIN_PA3, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
  { "sdmmc_bus1",         1, GPIO_PIN_PD2, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
  { "sdmmc_bus4",         1, GPIO_PIN_PD2, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
  { "sdmmc_bus4_1",       1, GPIO_PIN_PD3, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
  { "sdmmc_bus4_2",       1, GPIO_PIN_PD4, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
  { "sdmmc_bus4_3",       1, GPIO_PIN_PD5, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
};
STATIC CONST GPIO_IOMUX_CONFIG mSdmmc0IomuxPwrEnDefaultConfig[] = {
  { "sdmmc0_pwren",       0, GPIO_PIN_PA3, 1, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
};
STATIC CONST GPIO_IOMUX_CONFIG mSdmmc0IomuxPwrEnInvertedConfig[] = {
  { "sdmmc0_pwren",       0, GPIO_PIN_PA5, 0, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
};

typedef struct {
  UINT32                        Des0;
  UINT32                        Des1;
  UINT32                        Des2;
  UINT32                        Des3;
} DWEMMC_IDMAC_DESCRIPTOR;

EFI_MMC_HOST_PROTOCOL     *gpMmcHost;
DWEMMC_IDMAC_DESCRIPTOR   *gpIdmacDesc;
EFI_GUID mMshcDevicePathGuid = EFI_CALLER_ID_GUID;
STATIC UINT32 mMshcCommand;
STATIC UINT32 mMshcArgument;

EFI_STATUS
MshcReadBlockData (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN EFI_LBA                    Lba,
  IN UINTN                      Length,
  IN UINT32*                    Buffer
  );

BOOLEAN
MshcIsPowerOn (
  VOID
  )
{
    return TRUE;
}

EFI_STATUS
MshcInitialize (
  VOID
  )
{
    DEBUG ((DEBUG_BLKIO, "MshcInitialize()"));
    return EFI_SUCCESS;
}

BOOLEAN
MshcIsCardPresent (
  IN EFI_MMC_HOST_PROTOCOL     *This
  )
{
  return TRUE;
}

BOOLEAN
MshcIsReadOnly (
  IN EFI_MMC_HOST_PROTOCOL     *This
  )
{
  return FALSE;
}

BOOLEAN
MshcIsDmaSupported (
  IN EFI_MMC_HOST_PROTOCOL     *This
  )
{
  return TRUE;
}

EFI_STATUS
MshcBuildDevicePath (
  IN EFI_MMC_HOST_PROTOCOL      *This,
  IN EFI_DEVICE_PATH_PROTOCOL   **DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL *NewDevicePathNode;
  EFI_GUID DevicePathGuid = EFI_CALLER_ID_GUID;

  DEBUG ((DEBUG_VERBOSE, "MshcDxe: MMCBuildDevicePath()\n"));

  NewDevicePathNode = CreateDeviceNode (HARDWARE_DEVICE_PATH, HW_VENDOR_DP, sizeof (VENDOR_DEVICE_PATH));
  CopyGuid (&((VENDOR_DEVICE_PATH*) NewDevicePathNode)->Guid, &DevicePathGuid);
  *DevicePath = NewDevicePathNode;

  return EFI_SUCCESS;
}

EFI_STATUS
MshcUpdateClock (
  VOID
  )
{
  UINT32 Data;

  /* CMD_UPDATE_CLK */
  Data = BIT_CMD_WAIT_PRVDATA_COMPLETE | BIT_CMD_UPDATE_CLOCK_ONLY |
         BIT_CMD_START;
  MmioWrite32 (DWEMMC_CMD, Data);
  while (1) {
    Data = MmioRead32 (DWEMMC_CMD);
    if (!(Data & CMD_START_BIT)) {
      break;
    }
    Data = MmioRead32 (DWEMMC_RINTSTS);
    if (Data & DWEMMC_INT_HLE) {
      Print (L"failed to update mmc clock frequency\n");
      return EFI_DEVICE_ERROR;
    }
  }
  return EFI_SUCCESS;
}

EFI_STATUS
MshcSetClock (
  IN UINTN                     ClockFreq
  )
{
  UINT32 Divider, Rate, Data;
  EFI_STATUS Status;
  BOOLEAN Found = FALSE;

  Rate = CruGetSdmmcClockRate (0);
  if (Rate == ClockFreq) {
    Divider = 0;
  } else {
    for (Divider = 1; Divider < 256; Divider++) {
      if ((Rate / (2 * Divider)) <= ClockFreq) {
        Found = TRUE;
        break;
      }
    }
    if (Found == FALSE) {
      return EFI_NOT_FOUND;
    }
  }

  DEBUG ((DEBUG_INFO, "MshcSetClock(): ClockFreq = %lu Hz, Rate = %lu Hz, Divider = %u\n", ClockFreq, Rate, Divider));

  // Wait until MMC is idle
  do {
    Data = MmioRead32 (DWEMMC_STATUS);
  } while (Data & DWEMMC_STS_DATA_BUSY);

  // Disable MMC clock first
  MmioWrite32 (DWEMMC_CLKENA, 0);
  Status = MshcUpdateClock ();
  ASSERT (!EFI_ERROR (Status));

  MmioWrite32 (DWEMMC_CLKDIV, Divider);
  Status = MshcUpdateClock ();
  ASSERT (!EFI_ERROR (Status));

  // Enable MMC clock
  MmioWrite32 (DWEMMC_CLKENA, 1);
  MmioWrite32 (DWEMMC_CLKSRC, 0);
  Status = MshcUpdateClock ();
  ASSERT (!EFI_ERROR (Status));
  return EFI_SUCCESS;
}

EFI_STATUS
MshcNotifyState (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN MMC_STATE                 State
  )
{
  UINT32      Data;
  EFI_STATUS  Status;

  switch (State) {
  case MmcInvalidState:
    return EFI_INVALID_PARAMETER;
  case MmcHwInitializationState:
    MmioWrite32 (DWEMMC_PWREN, 1);

    // If device already turn on then restart it
    Data = DWEMMC_CTRL_RESET_ALL;
    MmioWrite32 (DWEMMC_CTRL, Data);
    do {
      // Wait until reset operation finished
      Data = MmioRead32 (DWEMMC_CTRL);
    } while (Data & DWEMMC_CTRL_RESET_ALL);

    // Setup clock that could not be higher than 400KHz.
    Status = MshcSetClock (400000);
    ASSERT (!EFI_ERROR (Status));
    // Wait clock stable
    MicroSecondDelay (100);

    MmioWrite32 (DWEMMC_RINTSTS, ~0);
    MmioWrite32 (DWEMMC_INTMASK, 0);
    MmioWrite32 (DWEMMC_TMOUT, ~0);
    MmioWrite32 (DWEMMC_IDINTEN, 0);
    MmioWrite32 (DWEMMC_CTYPE, 0);
    MmioWrite32 (DWEMMC_BMOD, DWEMMC_IDMAC_SWRESET);

    MmioWrite32 (DWEMMC_BLKSIZ, DWEMMC_BLOCK_SIZE);
    do {
      Data = MmioRead32 (DWEMMC_BMOD);
    } while (Data & DWEMMC_IDMAC_SWRESET);
    break;
  case MmcIdleState:
    break;
  case MmcReadyState:
    break;
  case MmcIdentificationState:
    break;
  case MmcStandByState:
    break;
  case MmcTransferState:
    break;
  case MmcSendingDataState:
    break;
  case MmcReceiveDataState:
    break;
  case MmcProgrammingState:
    break;
  case MmcDisconnectState:
    break;
  default:
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

// Need to prepare DMA buffer first before sending commands to MMC card
BOOLEAN
IsPendingReadCommand (
  IN MMC_CMD                    MmcCmd
  )
{
  UINTN  Mask;

  Mask = BIT_CMD_DATA_EXPECTED | BIT_CMD_READ;
  if ((MmcCmd & Mask) == Mask) {
    return TRUE;
  }
  return FALSE;
}

BOOLEAN
IsPendingWriteCommand (
  IN MMC_CMD                    MmcCmd
  )
{
  UINTN  Mask;

  Mask = BIT_CMD_DATA_EXPECTED | BIT_CMD_WRITE;
  if ((MmcCmd & Mask) == Mask) {
    return TRUE;
  }
  return FALSE;
}

EFI_STATUS
SendCommand (
  IN MMC_CMD                    MmcCmd,
  IN UINT32                     Argument
  )
{
  UINT32      Data, ErrMask;
  EFI_STATUS  Status = EFI_SUCCESS;

  // Wait until MMC is idle
  do {
    Data = MmioRead32 (DWEMMC_STATUS);
  } while (Data & DWEMMC_STS_DATA_BUSY);

  MmioWrite32 (DWEMMC_RINTSTS, ~0);
  MmioWrite32 (DWEMMC_CMDARG, Argument);
  MmioWrite32 (DWEMMC_CMD, MmcCmd);

  ErrMask = DWEMMC_INT_EBE | DWEMMC_INT_HLE | DWEMMC_INT_RTO |
            DWEMMC_INT_RCRC | DWEMMC_INT_RE;
  ErrMask |= DWEMMC_INT_DCRC | DWEMMC_INT_DRT | DWEMMC_INT_SBE;
  do {
    MicroSecondDelay(500);
    Data = MmioRead32 (DWEMMC_RINTSTS);

    if (Status == EFI_SUCCESS && (Data & ErrMask) != 0) {
      DEBUG ((DEBUG_INFO, "%a(): EFI_DEVICE_ERROR DWEMMC_RINTSTS=0x%x MmcCmd 0x%x(%d),Argument 0x%x\n",
          __func__, Data, MmcCmd, MmcCmd&0x3f, Argument));
      Status = EFI_DEVICE_ERROR;
      // Keep spinning until CMD done bit is set.
    }
    if (Data & DWEMMC_INT_DTO) {     // Transfer Done
      break;
    }
  } while (!(Data & DWEMMC_INT_CMD_DONE));
  return Status;
}

EFI_STATUS
MshcSendCommand (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN MMC_CMD                    MmcCmd,
  IN UINT32                     Argument
  )
{
  UINT32       Cmd = 0;
  EFI_STATUS   Status = EFI_SUCCESS;

  switch (MMC_GET_INDX(MmcCmd)) {
  case MMC_INDX(0):
    Cmd = BIT_CMD_SEND_INIT;
    break;
  case MMC_INDX(1):
    Cmd = BIT_CMD_RESPONSE_EXPECT;
    break;
  case MMC_INDX(2):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_LONG_RESPONSE |
           BIT_CMD_CHECK_RESPONSE_CRC | BIT_CMD_SEND_INIT;
    break;
  case MMC_INDX(3):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_SEND_INIT;
    break;
  case MMC_INDX(6):
    if (((Argument >> 31) & 0x1) == 0x1 || Argument == 0x00FFFFF0) {
      Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
            BIT_CMD_DATA_EXPECTED | BIT_CMD_READ |
            BIT_CMD_WAIT_PRVDATA_COMPLETE;
    } else {
      Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC;
    }
    break;
  case MMC_INDX(7):
    if (Argument) {
        Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC;
    } else {
        Cmd = 0;
    }
    break;
  case MMC_INDX(8):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_WAIT_PRVDATA_COMPLETE;
    break;
  case MMC_INDX(9):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_LONG_RESPONSE;
    break;
  case MMC_INDX(12):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_STOP_ABORT_CMD;
    break;
  case MMC_INDX(13):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_WAIT_PRVDATA_COMPLETE;
    break;
  case MMC_INDX(16):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC;
    break;
  case MMC_INDX(17):
  case MMC_INDX(18):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_DATA_EXPECTED | BIT_CMD_READ |
           BIT_CMD_WAIT_PRVDATA_COMPLETE;
    break;
  case MMC_INDX(24):
  case MMC_INDX(25):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_DATA_EXPECTED | BIT_CMD_WRITE |
           BIT_CMD_WAIT_PRVDATA_COMPLETE;
    break;
  case MMC_INDX(30):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_DATA_EXPECTED;
    break;
  case MMC_INDX(41):
    Cmd = BIT_CMD_RESPONSE_EXPECT;
    break;
  case MMC_INDX(51):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
          BIT_CMD_DATA_EXPECTED | BIT_CMD_READ |
          BIT_CMD_WAIT_PRVDATA_COMPLETE;
    break;
  default:
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC;
    break;
  }

  Cmd |= MMC_GET_INDX(MmcCmd) | BIT_CMD_USE_HOLD_REG | BIT_CMD_START;

  if (IsPendingReadCommand (Cmd) || IsPendingWriteCommand (Cmd)) {
    // DEBUG ((DEBUG_INFO, "%a(): Deferred Cmd=0x%08X(%u), Argument 0x%08X\n", __func__, MmcCmd, MMC_GET_INDX (MmcCmd), Argument));
    mMshcCommand = Cmd;
    mMshcArgument = Argument;
  } else {
    // DEBUG ((DEBUG_INFO, "%a(): Immediate Cmd=0x%08X(%u), Argument 0x%08X\n", __func__, MmcCmd, MMC_GET_INDX (MmcCmd)));
    Status = SendCommand (Cmd, Argument);
  }
  return Status;
}

EFI_STATUS
MshcReceiveResponse (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN MMC_RESPONSE_TYPE          Type,
  IN UINT32*                    Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (   (Type == MMC_RESPONSE_TYPE_R1)
      || (Type == MMC_RESPONSE_TYPE_R1b)
      || (Type == MMC_RESPONSE_TYPE_R3)
      || (Type == MMC_RESPONSE_TYPE_R6)
      || (Type == MMC_RESPONSE_TYPE_R7))
  {
    Buffer[0] = MmioRead32 (DWEMMC_RESP0);
  } else if (Type == MMC_RESPONSE_TYPE_R2) {
    Buffer[0] = MmioRead32 (DWEMMC_RESP0);
    Buffer[1] = MmioRead32 (DWEMMC_RESP1);
    Buffer[2] = MmioRead32 (DWEMMC_RESP2);
    Buffer[3] = MmioRead32 (DWEMMC_RESP3);
  }
  return EFI_SUCCESS;
}

VOID
MshcAdjustFifoThreshold (
  VOID
  )
{
  /* DMA multiple transaction size map to reg value as array index */
  CONST UINT32 BurstSize[] = {1, 4, 8, 16, 32, 64, 128, 256};
  UINT32 BlkDepthInFifo, FifoThreshold, FifoWidth, FifoDepth;
  UINT32 BlkSize = DWEMMC_BLOCK_SIZE, Idx = 0, RxWatermark = 1, TxWatermark, TxWatermarkInvers;

  /* Skip FIFO adjustment if we do not have platform FIFO depth info */
  FifoDepth = PcdGet32 (PcdMshcDxeFifoDepth);
  if (!FifoDepth) {
    return;
  }

  TxWatermark = FifoDepth / 2;
  TxWatermarkInvers = FifoDepth - TxWatermark;

  FifoWidth = DWEMMC_GET_HDATA_WIDTH (MmioRead32 (DWEMMC_HCON));
  if (!FifoWidth) {
    FifoWidth = 2;
  } else if (FifoWidth == 2) {
    FifoWidth = 8;
  } else {
    FifoWidth = 4;
  }

  BlkDepthInFifo = BlkSize / FifoWidth;

  Idx = ARRAY_SIZE (BurstSize) - 1;
  while (Idx && ((BlkDepthInFifo % BurstSize[Idx]) || (TxWatermarkInvers % BurstSize[Idx]))) {
    Idx--;
  }

  RxWatermark = BurstSize[Idx] - 1;
  FifoThreshold = DWEMMC_DMA_BURST_SIZE (Idx) | DWEMMC_FIFO_TWMARK (TxWatermark)
           | DWEMMC_FIFO_RWMARK (RxWatermark);
  MmioWrite32 (DWEMMC_FIFOTH, FifoThreshold);
}

EFI_STATUS
PrepareDmaData (
  IN DWEMMC_IDMAC_DESCRIPTOR*    IdmacDesc,
  IN UINTN                      Length,
  IN UINT32*                    Buffer
  )
{
  UINTN  Cnt, Blks, Idx, LastIdx;

  Cnt = (Length + DWEMMC_DMA_BUF_SIZE - 1) / DWEMMC_DMA_BUF_SIZE;
  Blks = (Length + DWEMMC_BLOCK_SIZE - 1) / DWEMMC_BLOCK_SIZE;
  Length = DWEMMC_BLOCK_SIZE * Blks;

  for (Idx = 0; Idx < Cnt; Idx++) {
    (IdmacDesc + Idx)->Des0 = DWEMMC_IDMAC_DES0_OWN | DWEMMC_IDMAC_DES0_CH |
                              DWEMMC_IDMAC_DES0_DIC;
    (IdmacDesc + Idx)->Des1 = DWEMMC_IDMAC_DES1_BS1(DWEMMC_DMA_BUF_SIZE);
    /* Buffer Address */
    (IdmacDesc + Idx)->Des2 = (UINT32)((UINTN)Buffer + DWEMMC_DMA_BUF_SIZE * Idx);
    /* Next Descriptor Address */
    (IdmacDesc + Idx)->Des3 = (UINT32)((UINTN)IdmacDesc +
                                       (sizeof(DWEMMC_IDMAC_DESCRIPTOR) * (Idx + 1)));
  }
  /* First Descriptor */
  IdmacDesc->Des0 |= DWEMMC_IDMAC_DES0_FS;
  /* Last Descriptor */
  LastIdx = Cnt - 1;
  (IdmacDesc + LastIdx)->Des0 |= DWEMMC_IDMAC_DES0_LD;
  (IdmacDesc + LastIdx)->Des0 &= ~(DWEMMC_IDMAC_DES0_DIC | DWEMMC_IDMAC_DES0_CH);
  (IdmacDesc + LastIdx)->Des1 = DWEMMC_IDMAC_DES1_BS1(Length -
                                                      (LastIdx * DWEMMC_DMA_BUF_SIZE));
  /* Set the Next field of Last Descriptor */
  (IdmacDesc + LastIdx)->Des3 = 0;
  MmioWrite32 (DWEMMC_DBADDR, (UINT32)((UINTN)IdmacDesc));

  return EFI_SUCCESS;
}

VOID
StartDma (
  UINTN    Length
  )
{
  UINT32 Data;

  Data = MmioRead32 (DWEMMC_CTRL);
  Data |= DWEMMC_CTRL_INT_EN | DWEMMC_CTRL_DMA_EN | DWEMMC_CTRL_IDMAC_EN;
  MmioWrite32 (DWEMMC_CTRL, Data);
  Data = MmioRead32 (DWEMMC_BMOD);
  Data |= DWEMMC_IDMAC_ENABLE | DWEMMC_IDMAC_FB;
  MmioWrite32 (DWEMMC_BMOD, Data);

  MmioWrite32 (DWEMMC_BLKSIZ, DWEMMC_BLOCK_SIZE);
  MmioWrite32 (DWEMMC_BYTCNT, Length);
}


EFI_STATUS
MshcReadBlockData (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN EFI_LBA                    Lba,
  IN UINTN                      Length,
  IN UINT32*                   Buffer
  )
{
  EFI_STATUS  Status;
  UINT32      DescPages, CountPerPage, Count;
  EFI_TPL     Tpl;

  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  CountPerPage = EFI_PAGE_SIZE / 16;
  Count = (Length + DWEMMC_DMA_BUF_SIZE - 1) / DWEMMC_DMA_BUF_SIZE;
  DescPages = (Count + CountPerPage - 1) / CountPerPage;

  InvalidateDataCacheRange (Buffer, Length);

  Status = PrepareDmaData (gpIdmacDesc, Length, Buffer);
  if (EFI_ERROR (Status)) {
    goto out;
  }

  WriteBackDataCacheRange (gpIdmacDesc, DescPages * EFI_PAGE_SIZE);
  StartDma (Length);

  Status = SendCommand (mMshcCommand, mMshcArgument);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to read data, mMshcCommand:%x, mMshcArgument:%x, Status:%r\n", mMshcCommand, mMshcArgument, Status));
    goto out;
  }
out:
  // Restore Tpl
  gBS->RestoreTPL (Tpl);
  return Status;
}

EFI_STATUS
MshcWriteBlockData (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN EFI_LBA                    Lba,
  IN UINTN                      Length,
  IN UINT32*                    Buffer
  )
{
  EFI_STATUS  Status;
  UINT32      DescPages, CountPerPage, Count;
  EFI_TPL     Tpl;

  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  CountPerPage = EFI_PAGE_SIZE / 16;
  Count = (Length + DWEMMC_DMA_BUF_SIZE - 1) / DWEMMC_DMA_BUF_SIZE;
  DescPages = (Count + CountPerPage - 1) / CountPerPage;

  WriteBackDataCacheRange (Buffer, Length);

  Status = PrepareDmaData (gpIdmacDesc, Length, Buffer);
  if (EFI_ERROR (Status)) {
    goto out;
  }

  WriteBackDataCacheRange (gpIdmacDesc, DescPages * EFI_PAGE_SIZE);
  StartDma (Length);

  Status = SendCommand (mMshcCommand, mMshcArgument);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to write data, mMshcCommand:%x, mMshcArgument:%x, Status:%r\n", mMshcCommand, mMshcArgument, Status));
    goto out;
  }
out:
  // Restore Tpl
  gBS->RestoreTPL (Tpl);
  return Status;
}

EFI_STATUS
MshcSetIos (
  IN EFI_MMC_HOST_PROTOCOL      *This,
  IN  UINT32                    BusClockFreq,
  IN  UINT32                    BusWidth,
  IN  UINT32                    TimingMode
  )
{
  EFI_STATUS Status = EFI_SUCCESS;
  UINT32    Data;

  if ((PcdGet32 (PcdMshcDxeMaxClockFreqInHz) != 0) &&
      (BusClockFreq > PcdGet32 (PcdMshcDxeMaxClockFreqInHz))) {
    return EFI_UNSUPPORTED;
  }
  if (TimingMode != EMMCBACKWARD) {
    Data = MmioRead32 (DWEMMC_UHSREG);
    switch (TimingMode) {
    case EMMCHS52DDR1V2:
    case EMMCHS52DDR1V8:
      Data |= 1 << 16;
      break;
    case EMMCHS52:
    case EMMCHS26:
      Data &= ~(1 << 16);
      break;
    default:
      return EFI_UNSUPPORTED;
    }
    MmioWrite32 (DWEMMC_UHSREG, Data);
  }

  switch (BusWidth) {
  case 1:
    MmioWrite32 (DWEMMC_CTYPE, 0);
    break;
  case 4:
    MmioWrite32 (DWEMMC_CTYPE, 1);
    break;
  case 8:
    MmioWrite32 (DWEMMC_CTYPE, 1 << 16);
    break;
  default:
    return EFI_UNSUPPORTED;
  }
  if (BusClockFreq) {
    Status = MshcSetClock (BusClockFreq);
  }
  return Status;
}

BOOLEAN
MshcIsMultiBlock (
  IN EFI_MMC_HOST_PROTOCOL      *This
  )
{
  return TRUE;
}

EFI_MMC_HOST_PROTOCOL gMciHost = {
  MMC_HOST_PROTOCOL_REVISION,
  MshcIsCardPresent,
  MshcIsReadOnly,
  MshcBuildDevicePath,
  MshcNotifyState,
  MshcSendCommand,
  MshcReceiveResponse,
  MshcReadBlockData,
  MshcWriteBlockData,
  MshcSetIos,
  MshcIsMultiBlock
};

EFI_STATUS
MshcDxeInitialize (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
{
  EFI_STATUS    Status;
  EFI_HANDLE    Handle;

  Handle = NULL;

  CruSetSdmmcClockRate (0, 100000000UL);

  CruAssertSoftReset (13, 4);
  MicroSecondDelay (5);
  CruDeassertSoftReset (13, 4);
  gpIdmacDesc = (DWEMMC_IDMAC_DESCRIPTOR *)AllocatePages (DWEMMC_MAX_DESC_PAGES);
  if (gpIdmacDesc == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }
  DEBUG ((DEBUG_BLKIO, "MshcDxeInitialize()\n"));

  /* Configure pins */
  GpioSetIomuxConfig (mSdmmc0IomuxConfig, ARRAY_SIZE (mSdmmc0IomuxConfig));

  MshcAdjustFifoThreshold ();

  //Publish Component Name, BlockIO protocol interfaces
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEmbeddedMmcHostProtocolGuid,    &gMciHost,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}

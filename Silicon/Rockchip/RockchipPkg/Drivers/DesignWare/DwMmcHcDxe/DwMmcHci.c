/** @file
  This driver is used to manage Designware SD/MMC host controllers.

  It would expose EFI_SD_MMC_PASS_THRU_PROTOCOL for upper layer use.

  Copyright (c) 2015 - 2021, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2018, Linaro Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <IndustryStandard/Emmc.h>
#include <IndustryStandard/Sd.h>

#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DmaLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "DwMmcHcDxe.h"

// Time to wait for the controller to take a command-register write, us
#define DW_MMC_CMD_START_TIMEOUT  500000

// Time to wait for the card to let go of DAT0 before the next command, us.
// A write can keep the card busy for 250 ms, an erase much longer.
#define DW_MMC_CARD_BUSY_TIMEOUT  5000000

// Time to wait for the auto-stop command after a multi-block transfer, us
#define DW_MMC_AUTO_STOP_TIMEOUT  200000

// Used when the caller gave no timeout: better a late error than a hang
#define DW_MMC_DEFAULT_TIMEOUT  (10 * 1000 * 1000)

#define DW_MMC_CMD_ERR_MASK  (DW_MMC_INT_RTO | DW_MMC_INT_RCRC | DW_MMC_INT_RE | DW_MMC_INT_HLE)
#define DW_MMC_DATA_ERR_MASK  (DW_MMC_INT_DCRC | DW_MMC_INT_DRT | DW_MMC_INT_HTO | \
                               DW_MMC_INT_FRUN | DW_MMC_INT_SBE | DW_MMC_INT_EBE)

/**
  Print the slot capabilities the platform driver reported.

  @param[in]  Slot            The slot number.
  @param[in]  Capability      The capabilities.

**/
VOID
DumpCapabilityReg (
  IN UINT8               Slot,
  IN DW_MMC_HC_SLOT_CAP  *Capability
  )
{
  DEBUG ((
    DEBUG_INFO,
    "DwMmc slot %d: %a, %a, %d-bit, input clock %d kHz, HS %a, 3.3V %a, 1.8V %a\n",
    Slot,
    (Capability->SlotType == RemovableSlot) ? "removable" : "embedded",
    (Capability->CardType == SdCardType) ? "SD" : "eMMC",
    Capability->BusWidth,
    Capability->BaseClkFreq,
    Capability->HighSpeed ? "yes" : "no",
    Capability->Voltage33 ? "yes" : "no",
    Capability->Voltage18 ? "yes" : "no"
    ));
}

/**
  Mask the controller interrupts and clear what is pending. The driver polls
  RINTSTS and IDSTS, which show the events whether they are masked or not.

  @param[in] DevBase        Controller registers.

  @retval EFI_SUCCESS       Done.

**/
EFI_STATUS
DwMmcHcEnableInterrupt (
  IN UINTN  DevBase
  )
{
  MmioWrite32 (DevBase + DW_MMC_INTMASK, 0);
  MmioWrite32 (DevBase + DW_MMC_RINTSTS, ~0U);
  MmioWrite32 (DevBase + DW_MMC_IDINTEN, ~0U);
  MmioWrite32 (DevBase + DW_MMC_IDSTS, ~0U);

  return EFI_SUCCESS;
}

EFI_STATUS
DwMmcHcGetCapability (
  IN     UINTN            DevBase,
  IN     EFI_HANDLE       Controller,
  IN     UINT8            Slot,
  OUT DW_MMC_HC_SLOT_CAP  *Capacity
  )
{
  PLATFORM_DW_MMC_PROTOCOL  *PlatformDwMmc;
  EFI_STATUS                Status;

  if (Capacity == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->LocateProtocol (
                  &gPlatformDwMmcProtocolGuid,
                  NULL,
                  (VOID **)&PlatformDwMmc
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return PlatformDwMmc->GetCapability (Controller, Slot, Capacity);
}

EFI_STATUS
DwMmcHcCardDetect (
  IN     UINTN       DevBase,
  IN     EFI_HANDLE  Controller,
  IN     UINT8       Slot,
  OUT BOOLEAN        *MediaPresent
  )
{
  PLATFORM_DW_MMC_PROTOCOL  *PlatformDwMmc;
  EFI_STATUS                Status;

  if (MediaPresent == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->LocateProtocol (
                  &gPlatformDwMmcProtocolGuid,
                  NULL,
                  (VOID **)&PlatformDwMmc
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *MediaPresent = PlatformDwMmc->CardDetect (Controller, Slot);
  return EFI_SUCCESS;
}

/**
  Wait for DAT0 to be released by the card.

  @param[in] DevBase        Controller registers.
  @param[in] TimeoutUs      How long to wait.

  @retval EFI_SUCCESS       The card is not busy.
  @retval EFI_TIMEOUT       Still busy.

**/
STATIC
EFI_STATUS
DwMmcHcWaitCardIdle (
  IN UINTN   DevBase,
  IN UINT32  TimeoutUs
  )
{
  UINT32  Waited;

  for (Waited = 0; Waited < TimeoutUs; Waited += 10) {
    if ((MmioRead32 (DevBase + DW_MMC_STATUS) & DW_MMC_STS_DATA_BUSY) == 0) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

/**
  Make the controller take over the clock settings (CLKDIV, CLKSRC, CLKENA).
  The databook asks for this after every change; it is a command that goes
  to the card's clock domain only, nothing is sent to the card.

  @param[in] DevBase        Controller registers.

  @retval EFI_SUCCESS       Taken over.
  @retval EFI_DEVICE_ERROR  Hardware lock error, the controller was busy.
  @retval EFI_TIMEOUT       The start bit did not clear.

**/
STATIC
EFI_STATUS
DwMmcHcUpdateClock (
  IN UINTN  DevBase
  )
{
  UINT32  Waited;

  MmioWrite32 (
    DevBase + DW_MMC_CMD,
    BIT_CMD_WAIT_PRVDATA_COMPLETE | BIT_CMD_UPDATE_CLOCK_ONLY | BIT_CMD_START
    );

  for (Waited = 0; Waited < DW_MMC_CMD_START_TIMEOUT; Waited++) {
    if ((MmioRead32 (DevBase + DW_MMC_CMD) & CMD_START_BIT) == 0) {
      return EFI_SUCCESS;
    }

    if ((MmioRead32 (DevBase + DW_MMC_RINTSTS) & DW_MMC_INT_HLE) != 0) {
      // HLE stays set until cleared: leaving it would fail every later try
      MmioWrite32 (DevBase + DW_MMC_RINTSTS, DW_MMC_INT_HLE);
      DEBUG ((DEBUG_ERROR, "DwMmcHcUpdateClock: hardware lock error\n"));
      return EFI_DEVICE_ERROR;
    }

    MicroSecondDelay (1);
  }

  DEBUG ((DEBUG_ERROR, "DwMmcHcUpdateClock: timed out\n"));
  return EFI_TIMEOUT;
}

/**
  Stop the card clock.

  @param[in] DevBase        Controller registers.

  @retval EFI_SUCCESS       Stopped.
  @retval Others            The clock update failed.

**/
EFI_STATUS
DwMmcHcStopClock (
  IN UINTN  DevBase
  )
{
  MmioWrite32 (DevBase + DW_MMC_CLKENA, 0);
  return DwMmcHcUpdateClock (DevBase);
}

/**
  Set the card clock and turn it on. The order is the one in the databook
  and the kernel driver: clock off, new divider, clock on, each followed by
  a clock update.

  @param[in] DevBase        Controller registers.
  @param[in] ClockFreq      Highest clock wanted, in kHz.
  @param[in] Capability     The slot capability (carries the input clock).

  @retval EFI_SUCCESS       The clock runs at or below ClockFreq.
  @retval Others            The controller did not take the change.

**/
EFI_STATUS
DwMmcHcClockSupply (
  IN UINTN               DevBase,
  IN UINT64              ClockFreq,
  IN DW_MMC_HC_SLOT_CAP  Capability
  )
{
  EFI_STATUS  Status;
  UINT32      Divisor;
  UINT32      Actual;
  UINT32      Try;

  if ((ClockFreq == 0) || (Capability.BaseClkFreq == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Divisor = DwMmcCalcClockDiv (
              Capability.BaseClkFreq,
              (ClockFreq > Capability.BaseClkFreq) ? Capability.BaseClkFreq : (UINT32)ClockFreq,
              &Actual
              );

  DEBUG ((
    DEBUG_INFO,
    "DwMmc clock: input %d kHz, divider %d, card clock %d kHz (wanted %ld)\n",
    Capability.BaseClkFreq,
    Divisor,
    Actual,
    ClockFreq
    ));

  Status = DwMmcHcWaitCardIdle (DevBase, DW_MMC_CARD_BUSY_TIMEOUT);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DwMmcHcClockSupply: card stays busy\n"));
    return Status;
  }

  Status = EFI_DEVICE_ERROR;
  for (Try = 0; Try < 3; Try++) {
    MmioWrite32 (DevBase + DW_MMC_CLKENA, 0);
    Status = DwMmcHcUpdateClock (DevBase);
    if (EFI_ERROR (Status)) {
      continue;
    }

    MmioWrite32 (DevBase + DW_MMC_CLKSRC, 0);
    MmioWrite32 (DevBase + DW_MMC_CLKDIV, Divisor);
    Status = DwMmcHcUpdateClock (DevBase);
    if (EFI_ERROR (Status)) {
      continue;
    }

    MmioWrite32 (DevBase + DW_MMC_CLKENA, 1);
    Status = DwMmcHcUpdateClock (DevBase);
    if (!EFI_ERROR (Status)) {
      break;
    }
  }

  return Status;
}

/**
  Set the data bus width and single or double data rate.

  @param[in] DevBase        Controller registers.
  @param[in] IsDdr          Dual data rate.
  @param[in] BusWidth       1, 4 or 8.

  @retval EFI_SUCCESS       Done.
  @retval EFI_INVALID_PARAMETER  Not a width the controller has.

**/
EFI_STATUS
DwMmcHcSetBusWidth (
  IN UINTN    DevBase,
  IN BOOLEAN  IsDdr,
  IN UINT16   BusWidth
  )
{
  UINT32  Ctype;
  UINT32  Uhs;

  switch (BusWidth) {
    case 1:
      Ctype = MMC_1BIT_MODE;
      break;
    case 4:
      Ctype = MMC_4BIT_MODE;
      break;
    case 8:
      Ctype = MMC_8BIT_MODE;
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  MmioWrite32 (DevBase + DW_MMC_CTYPE, Ctype);

  Uhs = MmioRead32 (DevBase + DW_MMC_UHSREG);
  if (IsDdr) {
    Uhs |= UHS_DDR_MODE;
  } else {
    Uhs &= ~UHS_DDR_MODE;
  }

  MmioWrite32 (DevBase + DW_MMC_UHSREG, Uhs);

  return EFI_SUCCESS;
}

/**
  Supply the card with the identification clock.

  @param[in] DevBase        Controller registers.
  @param[in] Capability     The slot capability.

  @retval EFI_SUCCESS       Running at 400 kHz or just below.
  @retval Others            The clock did not start.

**/
EFI_STATUS
DwMmcHcInitClockFreq (
  IN UINTN               DevBase,
  IN DW_MMC_HC_SLOT_CAP  Capability
  )
{
  EFI_STATUS  Status;

  if (Capability.BaseClkFreq == 0) {
    return EFI_UNSUPPORTED;
  }

  Status = DwMmcHcClockSupply (DevBase, DWMMC_INIT_CLOCK_FREQ, Capability);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // The card wants 74 clocks before the first command; 100 us is 40 at 400 kHz
  // and the init flag on CMD0 adds 80 more.
  MicroSecondDelay (200);
  return EFI_SUCCESS;
}

/**
  Reset parts of the controller and wait for the reset bits to clear.

  @param[in] DevBase        Controller registers.
  @param[in] ResetValue     DW_MMC_CTRL_* reset bits.

  @retval EFI_SUCCESS       Done.
  @retval EFI_TIMEOUT       A reset bit did not clear.

**/
STATIC
EFI_STATUS
DwMmcHcWaitReset (
  IN UINTN   DevBase,
  IN UINT32  ResetValue
  )
{
  UINT32  Timeout;

  MmioWrite32 (DevBase + DW_MMC_CTRL, MmioRead32 (DevBase + DW_MMC_CTRL) | ResetValue);

  for (Timeout = 0; Timeout < DW_MMC_HC_GENERIC_TIMEOUT; Timeout++) {
    if ((MmioRead32 (DevBase + DW_MMC_CTRL) & ResetValue) == 0) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

/**
  Program the response and data timeouts and the card detect debounce.

  @param[in] DevBase        Controller registers.

  @retval EFI_SUCCESS       Done.

**/
EFI_STATUS
DwMmcHcInitTimeoutCtrl (
  IN UINTN  DevBase
  )
{
  // Data timeout 0xFFFFFF card clocks (as long as it goes), response 0xFF
  MmioWrite32 (DevBase + DW_MMC_TMOUT, ~0U);
  MmioWrite32 (DevBase + DW_MMC_DEBNCE, 0x00FFFFFF);

  return EFI_SUCCESS;
}

/**
  Reset the controller, find out which register layout it has and make it
  ready for a card. Also resets the IDMAC and points it at the descriptors.

  @param[in] Private        The controller.

  @retval EFI_SUCCESS       Ready.
  @retval Others            The reset did not finish.

**/
EFI_STATUS
DwMmcHcInitHost (
  IN DW_MMC_HC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;
  UINTN       DevBase;
  UINT32      Hcon;
  UINT32      Bmod;
  UINT32      Timeout;

  DevBase = Private->DevBase;

  MmioWrite32 (DevBase + DW_MMC_PWREN, 1);

  Status = DwMmcHcWaitReset (DevBase, DW_MMC_CTRL_RESET_ALL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DwMmcHcInitHost: reset timed out\n"));
    return Status;
  }

  // The reset does not bring the card clock domain back by itself
  Status = DwMmcHcUpdateClock (DevBase);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->ControllerVersion = MmioRead32 (DevBase + DW_MMC_VERID) & 0xFFFF;
  Private->FifoOffset        = (Private->ControllerVersion < 0x240A) ?
                               DW_MMC_FIFO_START_OLD : DW_MMC_FIFO_START_240A;
  Hcon = MmioRead32 (DevBase + DW_MMC_HCON);
  DEBUG ((
    DEBUG_INFO,
    "DwMmc at 0x%lx: version 0x%04x, HCON 0x%08x (data width %d), FIFO port +0x%x\n",
    (UINT64)DevBase,
    Private->ControllerVersion,
    Hcon,
    ((Hcon >> 7) & 7) == 1 ? 32 : (((Hcon >> 7) & 7) == 2 ? 64 : 16),
    (UINT32)Private->FifoOffset
    ));

  // IDMAC: software reset, then it waits for descriptors
  Bmod = MmioRead32 (DevBase + DW_MMC_BMOD);
  MmioWrite32 (DevBase + DW_MMC_BMOD, Bmod | DW_MMC_BMOD_SWR);
  for (Timeout = 0; Timeout < 1000; Timeout++) {
    if ((MmioRead32 (DevBase + DW_MMC_BMOD) & DW_MMC_BMOD_SWR) == 0) {
      break;
    }

    MicroSecondDelay (1);
  }

  if (Private->IdmacDesc != NULL) {
    MmioWrite32 (DevBase + DW_MMC_DBADDR, (UINT32)Private->IdmacDescPhys);
  }

  // The card clock stays off until the first DwMmcHcClockSupply
  MmioWrite32 (DevBase + DW_MMC_CLKENA, 0);
  MmioWrite32 (DevBase + DW_MMC_CLKSRC, 0);
  MmioWrite32 (DevBase + DW_MMC_CTRL, MmioRead32 (DevBase + DW_MMC_CTRL) | DW_MMC_CTRL_INT_EN);

  return EFI_SUCCESS;
}

/**
  Switch the controller off: stop the card clock and the IDMAC.

  @param[in] Private        The controller.

**/
VOID
DwMmcHcShutdown (
  IN DW_MMC_HC_PRIVATE_DATA  *Private
  )
{
  UINTN   DevBase;
  UINT32  Ctrl;

  DevBase = Private->DevBase;

  Ctrl  = MmioRead32 (DevBase + DW_MMC_CTRL);
  Ctrl &= ~(DW_MMC_CTRL_IDMAC_EN | DW_MMC_CTRL_DMA_EN);
  MmioWrite32 (DevBase + DW_MMC_CTRL, Ctrl);
  MmioWrite32 (DevBase + DW_MMC_BMOD, MmioRead32 (DevBase + DW_MMC_BMOD) & ~(DW_MMC_BMOD_DE | DW_MMC_BMOD_FB));
  DwMmcHcStopClock (DevBase);
}

/**
  Allocate the IDMAC descriptor chain: uncached, so the controller sees what
  the CPU wrote without cache maintenance. Done once per controller.

  @param[in] Private        The controller.

**/
VOID
DwMmcHcAllocateDma (
  IN DW_MMC_HC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;
  UINTN       Bytes;
  VOID        *Buffer;

  Private->IdmacDescPages = EFI_SIZE_TO_PAGES (DW_MMC_IDMAC_MAX_DESC * sizeof (DW_MMC_HC_DMA_DESC_LINE));
  Bytes                   = EFI_PAGES_TO_SIZE (Private->IdmacDescPages);

  Status = DmaAllocateBuffer (EfiBootServicesData, Private->IdmacDescPages, &Buffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DwMmc: no memory for the DMA descriptors (%r), using the FIFO only\n", Status));
    return;
  }

  ZeroMem (Buffer, Bytes);

  Status = DmaMap (
             MapOperationBusMasterCommonBuffer,
             Buffer,
             &Bytes,
             0,
             0,
             &Private->IdmacDescPhys,
             &Private->IdmacDescMap
             );
  if (EFI_ERROR (Status) || (Bytes != EFI_PAGES_TO_SIZE (Private->IdmacDescPages)) ||
      (Private->IdmacDescPhys >= 0x100000000ULL) ||
      ((Private->IdmacDescPhys + Bytes) > 0x100000000ULL))
  {
    DEBUG ((DEBUG_ERROR, "DwMmc: DMA descriptors can't be mapped (%r), using the FIFO only\n", Status));
    if (!EFI_ERROR (Status)) {
      DmaUnmap (Private->IdmacDescMap);
    }

    DmaFreeBuffer (Private->IdmacDescPages, Buffer);
    Private->IdmacDescMap = NULL;
    return;
  }

  Private->IdmacDesc = Buffer;
}

/**
  Free what DwMmcHcAllocateDma allocated.

  @param[in] Private        The controller.

**/
VOID
DwMmcHcFreeDma (
  IN DW_MMC_HC_PRIVATE_DATA  *Private
  )
{
  if (Private->IdmacDesc == NULL) {
    return;
  }

  DmaUnmap (Private->IdmacDescMap);
  DmaFreeBuffer (Private->IdmacDescPages, Private->IdmacDesc);
  Private->IdmacDesc    = NULL;
  Private->IdmacDescMap = NULL;
}

/**
  Point the IDMAC at the descriptors for this transfer and let it go. The
  controller starts moving data once the command is issued.

  @param[in] Private        The controller.
  @param[in] Trb            The transfer.

  @retval EFI_SUCCESS       Armed.
  @retval EFI_BAD_BUFFER_SIZE  The buffer does not fit in the descriptor chain.

**/
STATIC
EFI_STATUS
DwMmcHcStartDma (
  IN DW_MMC_HC_PRIVATE_DATA  *Private,
  IN DW_MMC_HC_TRB           *Trb
  )
{
  UINTN   DevBase;
  UINT32  Count;
  UINT32  Ctrl;
  UINT32  Bmod;

  DevBase = Private->DevBase;

  Count = DwMmcFillDescriptors (
            Private->IdmacDesc,
            DW_MMC_IDMAC_MAX_DESC,
            (UINT32)Private->IdmacDescPhys,
            Trb->DataPhy,
            Trb->DataLen
            );
  if (Count == 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  // The descriptors are uncached memory; make sure they are out before the
  // controller is told to read them.
  ArmDataSynchronizationBarrier ();

  MmioWrite32 (DevBase + DW_MMC_IDSTS, ~0U);
  MmioWrite32 (DevBase + DW_MMC_DBADDR, (UINT32)Private->IdmacDescPhys);

  // The IDMAC takes the data, and the controller must be told to use DMA
  Ctrl = MmioRead32 (DevBase + DW_MMC_CTRL);
  Ctrl |= DW_MMC_CTRL_IDMAC_EN | DW_MMC_CTRL_DMA_EN;
  MmioWrite32 (DevBase + DW_MMC_CTRL, Ctrl);

  Bmod = MmioRead32 (DevBase + DW_MMC_BMOD);
  Bmod |= DW_MMC_BMOD_DE | DW_MMC_BMOD_FB;
  MmioWrite32 (DevBase + DW_MMC_BMOD, Bmod);

  // Wake the descriptor fetch
  MmioWrite32 (DevBase + DW_MMC_PLDMND, 1);

  return EFI_SUCCESS;
}

/**
  Turn the IDMAC off and drop what is left in the FIFO and the DMA engine.

  @param[in] Private        The controller.

**/
STATIC
VOID
DwMmcHcStopDma (
  IN DW_MMC_HC_PRIVATE_DATA  *Private
  )
{
  UINTN   DevBase;
  UINT32  Ctrl;
  UINT32  Bmod;

  DevBase = Private->DevBase;

  Ctrl  = MmioRead32 (DevBase + DW_MMC_CTRL);
  Ctrl &= ~(DW_MMC_CTRL_IDMAC_EN | DW_MMC_CTRL_DMA_EN);
  MmioWrite32 (DevBase + DW_MMC_CTRL, Ctrl);

  Bmod  = MmioRead32 (DevBase + DW_MMC_BMOD);
  Bmod &= ~(DW_MMC_BMOD_DE | DW_MMC_BMOD_FB);
  MmioWrite32 (DevBase + DW_MMC_BMOD, Bmod);

  DwMmcHcWaitReset (DevBase, DW_MMC_CTRL_DMA_RESET | DW_MMC_CTRL_FIFO_RESET);
}

/**
  Move data between the buffer and the FIFO by hand: for the short transfers
  (SCR, switch status) and as the fallback when there is no IDMAC.

  @param[in] Trb            The transfer.
  @param[in] BudgetUs       How long without progress is too long.

  @retval EFI_SUCCESS       All data moved and the controller reports data over.
  @retval EFI_TIMEOUT       Stalled.
  @retval EFI_DEVICE_ERROR  The controller reported a data error.

**/
STATIC
EFI_STATUS
DwMmcHcTransferFifo (
  IN DW_MMC_HC_TRB  *Trb,
  IN UINT32         BudgetUs
  )
{
  UINTN   DevBase;
  UINT32  Total;
  UINT32  Done;
  UINT32  Count;
  UINT32  Index;
  UINT32  Sts;
  UINT32  Intsts;
  UINT32  Word;
  UINT32  Idle;
  UINT8   *Buf;
  UINTN   Fifo;

  DevBase = Trb->Private->DevBase;
  Fifo    = DevBase + Trb->Private->FifoOffset;
  Buf     = Trb->Data;
  Total   = (Trb->DataLen + 3) / 4;
  Done    = 0;
  Idle    = 0;

  for ( ; ;) {
    Intsts = MmioRead32 (DevBase + DW_MMC_RINTSTS);
    if ((Intsts & DW_MMC_DATA_ERR_MASK) != 0) {
      DEBUG ((
        DEBUG_ERROR,
        "DwMmc FIFO: data error 0x%x after %d of %d words\n",
        Intsts,
        Done,
        Total
        ));
      return EFI_DEVICE_ERROR;
    }

    Count = 0;
    if (Trb->Read) {
      if ((Intsts & (DW_MMC_INT_RXDR | DW_MMC_INT_DTO)) != 0) {
        Sts   = MmioRead32 (DevBase + DW_MMC_STATUS);
        Count = GET_STS_FIFO_COUNT (Sts);
        if (Count > (Total - Done)) {
          Count = Total - Done;
        }

        for (Index = 0; Index < Count; Index++) {
          Word = MmioRead32 (Fifo);
          CopyMem (Buf + (Done + Index) * 4, &Word, MIN (4, Trb->DataLen - (Done + Index) * 4));
        }

        MmioWrite32 (DevBase + DW_MMC_RINTSTS, DW_MMC_INT_RXDR);
      }
    } else if (Done < Total) {
      Sts = MmioRead32 (DevBase + DW_MMC_STATUS);
      if (!DW_MMC_STS_FIFO_FULL (Sts)) {
        Count = DW_MMC_FIFO_DEPTH - GET_STS_FIFO_COUNT (Sts);
        if (Count > (Total - Done)) {
          Count = Total - Done;
        }

        for (Index = 0; Index < Count; Index++) {
          Word = 0;
          CopyMem (&Word, Buf + (Done + Index) * 4, MIN (4, Trb->DataLen - (Done + Index) * 4));
          MmioWrite32 (Fifo, Word);
        }

        MmioWrite32 (DevBase + DW_MMC_RINTSTS, DW_MMC_INT_TXDR);
      }
    }

    Done += Count;

    if (Done >= Total) {
      if ((Intsts & DW_MMC_INT_DTO) != 0) {
        return EFI_SUCCESS;
      }
    } else if ((Intsts & DW_MMC_INT_DTO) != 0) {
      if (Trb->Read && (Count == 0)) {
        // Data over, FIFO empty, buffer not full: the card sent less
        DEBUG ((DEBUG_ERROR, "DwMmc FIFO: short transfer, %d of %d words\n", Done, Total));
        return EFI_DEVICE_ERROR;
      }
    }

    if (Count != 0) {
      Idle = 0;
    } else {
      if (++Idle > BudgetUs) {
        DEBUG ((
          DEBUG_ERROR,
          "DwMmc FIFO: stalled at %d of %d words, RINTSTS 0x%x\n",
          Done,
          Total,
          Intsts
          ));
        return EFI_TIMEOUT;
      }

      MicroSecondDelay (1);
    }
  }
}

/**
  Wait for the IDMAC transfer: the controller says the data is over and the
  IDMAC says it finished the last descriptor.

  @param[in] Trb            The transfer.
  @param[in] BudgetUs       Time allowed.

  @retval EFI_SUCCESS       Done.
  @retval EFI_TIMEOUT       Took too long.
  @retval EFI_DEVICE_ERROR  Data or DMA error.

**/
STATIC
EFI_STATUS
DwMmcHcWaitDma (
  IN DW_MMC_HC_TRB  *Trb,
  IN UINT32         BudgetUs
  )
{
  UINTN   DevBase;
  UINT32  Waited;
  UINT32  Intsts;
  UINT32  Idsts;
  UINT32  Want;

  DevBase = Trb->Private->DevBase;
  Want    = Trb->Read ? DW_MMC_IDSTS_RI : DW_MMC_IDSTS_TI;

  for (Waited = 0; Waited < BudgetUs; Waited += 5) {
    Intsts = MmioRead32 (DevBase + DW_MMC_RINTSTS);
    Idsts  = MmioRead32 (DevBase + DW_MMC_IDSTS);

    if ((Intsts & DW_MMC_DATA_ERR_MASK) != 0) {
      DEBUG ((DEBUG_ERROR, "DwMmc DMA: data error, RINTSTS 0x%x IDSTS 0x%x\n", Intsts, Idsts));
      return EFI_DEVICE_ERROR;
    }

    if ((Idsts & DW_MMC_IDSTS_FATAL) != 0) {
      DEBUG ((DEBUG_ERROR, "DwMmc DMA: IDMAC error, RINTSTS 0x%x IDSTS 0x%x\n", Intsts, Idsts));
      return EFI_DEVICE_ERROR;
    }

    if (((Intsts & DW_MMC_INT_DTO) != 0) && ((Idsts & Want) != 0)) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (5);
  }

  DEBUG ((
    DEBUG_ERROR,
    "DwMmc DMA: timed out, RINTSTS 0x%x IDSTS 0x%x STATUS 0x%x\n",
    MmioRead32 (DevBase + DW_MMC_RINTSTS),
    MmioRead32 (DevBase + DW_MMC_IDSTS),
    MmioRead32 (DevBase + DW_MMC_STATUS)
    ));
  return EFI_TIMEOUT;
}

/**
  Send CMD12 by hand to get a card out of a transfer that went wrong.
  The result does not matter: the card is reinitialised if it stays stuck.

  @param[in] DevBase        Controller registers.

**/
STATIC
VOID
DwMmcHcAbortTransfer (
  IN UINTN  DevBase
  )
{
  UINT32  Waited;

  MmioWrite32 (DevBase + DW_MMC_RINTSTS, ~0U);
  MmioWrite32 (DevBase + DW_MMC_CMDARG, 0);
  MmioWrite32 (
    DevBase + DW_MMC_CMD,
    DwMmcBuildCmd (12, SdMmcCommandTypeAc, SdMmcResponseTypeR1b, DwMmcDataNone) | BIT_CMD_START
    );

  for (Waited = 0; Waited < 100000; Waited++) {
    if ((MmioRead32 (DevBase + DW_MMC_RINTSTS) & (DW_MMC_INT_CMD_DONE | DW_MMC_CMD_ERR_MASK)) != 0) {
      break;
    }

    MicroSecondDelay (1);
  }
}

/**
  Create a new TRB for the SD/MMC cmd request. The data buffer is mapped for
  DMA here (this is where the cache is cleaned or invalidated, and where a
  buffer the controller can't reach gets a bounce buffer).

  @param[in] Private        A pointer to the DW_MMC_HC_PRIVATE_DATA instance.
  @param[in] Slot           The slot number of the SD card to send the command
                            to.
  @param[in] Packet         A pointer to the SD command data structure.
  @param[in] Event          If Event is NULL, blocking I/O is performed. If
                            Event is not NULL, then nonblocking I/O is
                            performed, and Event will be signaled when the
                            Packet completes.

  @return Created Trb or NULL.

**/
DW_MMC_HC_TRB *
DwMmcCreateTrb (
  IN DW_MMC_HC_PRIVATE_DATA               *Private,
  IN UINT8                                Slot,
  IN EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  *Packet,
  IN EFI_EVENT                            Event
  )
{
  DW_MMC_HC_TRB  *Trb;
  EFI_STATUS     Status;
  EFI_TPL        OldTpl;
  UINTN          MapLength;

  Trb = AllocateZeroPool (sizeof (DW_MMC_HC_TRB));
  if (Trb == NULL) {
    return NULL;
  }

  Trb->Signature = DW_MMC_HC_TRB_SIG;
  Trb->Slot      = Slot;
  Trb->Packet    = Packet;
  Trb->Event     = Event;
  Trb->Started   = FALSE;
  Trb->Timeout   = Packet->Timeout;
  Trb->Private   = Private;

  if ((Packet->InTransferLength != 0) && (Packet->InDataBuffer != NULL)) {
    Trb->Data    = Packet->InDataBuffer;
    Trb->DataLen = Packet->InTransferLength;
    Trb->Read    = TRUE;
    ZeroMem (Trb->Data, Trb->DataLen);
  } else if ((Packet->OutTransferLength != 0) && (Packet->OutDataBuffer != NULL)) {
    Trb->Data    = Packet->OutDataBuffer;
    Trb->DataLen = Packet->OutTransferLength;
    Trb->Read    = FALSE;
  } else if ((Packet->InTransferLength == 0) && (Packet->OutTransferLength == 0)) {
    Trb->Data    = NULL;
    Trb->DataLen = 0;
  } else {
    goto Error;
  }

  // Short transfers and controllers without a descriptor chain use the FIFO
  Trb->UseDma = (Trb->DataLen >= DW_MMC_DMA_MIN_BYTES) && ((Trb->DataLen & 3) == 0) &&
                (Private->IdmacDesc != NULL) && !FixedPcdGetBool (PcdDwMmcForcePio);

  if (Trb->UseDma) {
    MapLength = Trb->DataLen;
    Status    = DmaMap (
                  Trb->Read ? MapOperationBusMasterWrite : MapOperationBusMasterRead,
                  Trb->Data,
                  &MapLength,
                  0,
                  0,
                  &Trb->DataPhy,
                  &Trb->DataMap
                  );
    if (EFI_ERROR (Status) || (Trb->DataLen != MapLength)) {
      DEBUG ((DEBUG_ERROR, "DwMmc: can't map %d bytes for DMA: %r\n", Trb->DataLen, Status));
      if (!EFI_ERROR (Status)) {
        DmaUnmap (Trb->DataMap);
        Trb->DataMap = NULL;
      }

      goto Error;
    }

    if (!DwMmcDmaReachable (Trb->DataPhy, Trb->DataLen)) {
      // Above 4 GiB, not word aligned, or more than one command can move
      DEBUG ((DEBUG_ERROR, "DwMmc: buffer 0x%lx, %d bytes is out of reach for the IDMAC\n", Trb->DataPhy, Trb->DataLen));
      DmaUnmap (Trb->DataMap);
      Trb->DataMap = NULL;
      goto Error;
    }
  }

  if (Event != NULL) {
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    InsertTailList (&Private->Queue, &Trb->TrbList);
    gBS->RestoreTPL (OldTpl);
  }

  return Trb;

Error:
  FreePool (Trb);
  return NULL;
}

/**
  Free the resource used by the TRB. This is where a bounce buffer is copied
  back for reads.

  @param[in] Trb            The pointer to the DW_MMC_HC_TRB instance.

**/
VOID
DwMmcFreeTrb (
  IN DW_MMC_HC_TRB  *Trb
  )
{
  if (Trb->DataMap != NULL) {
    DmaUnmap (Trb->DataMap);
  }

  FreePool (Trb);
}

/**
  Check if the env is ready for execute specified TRB.

  @retval EFI_SUCCESS       Always: commands are run one at a time.

**/
EFI_STATUS
DwMmcCheckTrbEnv (
  IN DW_MMC_HC_PRIVATE_DATA  *Private,
  IN DW_MMC_HC_TRB           *Trb
  )
{
  return EFI_SUCCESS;
}

/**
  Wait for the env to be ready for execute specified TRB.

  @retval EFI_SUCCESS       Always, see DwMmcCheckTrbEnv.

**/
EFI_STATUS
DwMmcWaitTrbEnv (
  IN DW_MMC_HC_PRIVATE_DATA  *Private,
  IN DW_MMC_HC_TRB           *Trb
  )
{
  return DwMmcCheckTrbEnv (Private, Trb);
}

/**
  Run one command, data included, to the end.

  @param[in] Private        A pointer to the DW_MMC_HC_PRIVATE_DATA instance.
  @param[in] Trb            The pointer to the DW_MMC_HC_TRB instance.

  @retval EFI_SUCCESS       Command and data done, response stored.
  @retval EFI_TIMEOUT       The card did not answer, or the data stalled.
  @retval EFI_DEVICE_ERROR  CRC, protocol or DMA error.

**/
EFI_STATUS
DwMmcExecTrb (
  IN DW_MMC_HC_PRIVATE_DATA  *Private,
  IN DW_MMC_HC_TRB           *Trb
  )
{
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  *Packet;
  EFI_STATUS                           Status;
  UINTN                                DevBase;
  UINT32                               Cmd;
  UINT32                               IntStatus;
  UINT32                               Waited;
  UINT32                               BudgetUs;
  UINT32                               BlkSize;
  DW_MMC_DATA_DIR                      Dir;
  BOOLEAN                              AutoStop;

  Packet     = Trb->Packet;
  DevBase    = Private->DevBase;

  BudgetUs = (Packet->Timeout != 0) ? (UINT32)MIN (Packet->Timeout, (UINT64)0xFFFFFFF0) : DW_MMC_DEFAULT_TIMEOUT;

  if (Trb->DataLen == 0) {
    Dir = DwMmcDataNone;
  } else {
    Dir = Trb->Read ? DwMmcDataRead : DwMmcDataWrite;
  }

  Cmd      = DwMmcBuildCmd (Packet->SdMmcCmdBlk->CommandIndex, Packet->SdMmcCmdBlk->CommandType, Packet->SdMmcCmdBlk->ResponseType, Dir);
  AutoStop = (Cmd & BIT_CMD_SEND_AUTO_STOP) != 0;

  //
  // The controller takes one command at a time, and (except for a stop)
  // only when the card is done with the last one.
  //
  for (Waited = 0; (MmioRead32 (DevBase + DW_MMC_CMD) & CMD_START_BIT) != 0; Waited++) {
    if (Waited > DW_MMC_CMD_START_TIMEOUT) {
      DEBUG ((DEBUG_ERROR, "DwMmc: previous command never finished\n"));
      return EFI_DEVICE_ERROR;
    }

    MicroSecondDelay (1);
  }

  if ((Cmd & BIT_CMD_STOP_ABORT_CMD) == 0) {
    Status = DwMmcHcWaitCardIdle (DevBase, DW_MMC_CARD_BUSY_TIMEOUT);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "DwMmc: CMD%d: card still busy, STATUS 0x%x\n", Packet->SdMmcCmdBlk->CommandIndex, MmioRead32 (DevBase + DW_MMC_STATUS)));
      return Status;
    }
  }

  MmioWrite32 (DevBase + DW_MMC_RINTSTS, ~0U);
  MmioWrite32 (DevBase + DW_MMC_IDSTS, ~0U);

  if (Trb->DataLen != 0) {
    BlkSize = (Trb->DataLen < DW_MMC_BLOCK_SIZE) ? Trb->DataLen : DW_MMC_BLOCK_SIZE;
    MmioWrite32 (DevBase + DW_MMC_BLKSIZ, BlkSize);
    MmioWrite32 (DevBase + DW_MMC_BYTCNT, Trb->DataLen);
    MmioWrite32 (DevBase + DW_MMC_FIFOTH, DwMmcCalcFifoth (BlkSize, Trb->UseDma));

    Status = DwMmcHcWaitReset (DevBase, DW_MMC_CTRL_FIFO_RESET);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "DwMmc: FIFO reset timed out\n"));
      return Status;
    }

    if (Trb->UseDma) {
      Status = DwMmcHcStartDma (Private, Trb);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  MmioWrite32 (DevBase + DW_MMC_CMDARG, Packet->SdMmcCmdBlk->CommandArgument);
  ArmDataSynchronizationBarrier ();
  MmioWrite32 (DevBase + DW_MMC_CMD, Cmd | BIT_CMD_START);

  //
  // Command phase
  //
  Status = EFI_TIMEOUT;
  for (Waited = 0; Waited < BudgetUs; Waited++) {
    IntStatus = MmioRead32 (DevBase + DW_MMC_RINTSTS);

    if ((IntStatus & DW_MMC_INT_RTO) != 0) {
      Status = EFI_TIMEOUT;
      break;
    }

    if ((IntStatus & DW_MMC_CMD_ERR_MASK) != 0) {
      Status = EFI_DEVICE_ERROR;
      break;
    }

    if ((IntStatus & DW_MMC_INT_CMD_DONE) != 0) {
      Status = EFI_SUCCESS;
      break;
    }

    MicroSecondDelay (1);
  }

  if (EFI_ERROR (Status)) {
    // Cards that don't answer are the normal case while probing (CMD8, CMD1
    // on an SD card); anything else is worth a line.
    DEBUG ((
      (Status == EFI_TIMEOUT) ? DEBUG_VERBOSE : DEBUG_ERROR,
      "DwMmc: CMD%d failed: %r, RINTSTS 0x%x\n",
      Packet->SdMmcCmdBlk->CommandIndex,
      Status,
      IntStatus
      ));
    if ((IntStatus & DW_MMC_INT_HLE) != 0) {
      MmioWrite32 (DevBase + DW_MMC_RINTSTS, DW_MMC_INT_HLE);
    }

    goto Done;
  }

  //
  // Response. The CSD and CID registers come without the CRC byte in an SD
  // host controller; the callers expect that layout, so shift by a byte.
  //
  if (Packet->SdMmcCmdBlk->CommandType != SdMmcCommandTypeBc) {
    Packet->SdMmcStatusBlk->Resp0 = MmioRead32 (DevBase + DW_MMC_RESP0);
    if (Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR2) {
      Packet->SdMmcStatusBlk->Resp1 = MmioRead32 (DevBase + DW_MMC_RESP1);
      Packet->SdMmcStatusBlk->Resp2 = MmioRead32 (DevBase + DW_MMC_RESP2);
      Packet->SdMmcStatusBlk->Resp3 = MmioRead32 (DevBase + DW_MMC_RESP3);

      if ((Packet->SdMmcCmdBlk->CommandIndex == SD_SEND_CSD) ||
          (Packet->SdMmcCmdBlk->CommandIndex == SD_SEND_CID))
      {
        UINT32  Buf[4];
        ZeroMem (Buf, sizeof (Buf));
        CopyMem ((UINT8 *)Buf, (UINT8 *)&Packet->SdMmcStatusBlk->Resp0 + 1, sizeof (Buf) - 1);
        CopyMem ((UINT8 *)&Packet->SdMmcStatusBlk->Resp0, (UINT8 *)Buf, sizeof (Buf) - 1);
      }
    }
  }

  //
  // Data phase
  //
  if (Trb->DataLen != 0) {
    if (Trb->UseDma) {
      Status = DwMmcHcWaitDma (Trb, BudgetUs);
    } else {
      Status = DwMmcHcTransferFifo (Trb, BudgetUs);
    }

    if (!EFI_ERROR (Status) && AutoStop) {
      // The controller sends CMD12 itself once the byte count is reached
      for (Waited = 0; Waited < DW_MMC_AUTO_STOP_TIMEOUT; Waited++) {
        if ((MmioRead32 (DevBase + DW_MMC_RINTSTS) & DW_MMC_INT_ACD) != 0) {
          break;
        }

        MicroSecondDelay (1);
      }

      if (Waited >= DW_MMC_AUTO_STOP_TIMEOUT) {
        // Not fatal: the data is in. Reported so a hardware log shows it.
        DEBUG ((DEBUG_WARN, "DwMmc: auto stop not reported done, RINTSTS 0x%x\n", MmioRead32 (DevBase + DW_MMC_RINTSTS)));
      }
    }

    if (EFI_ERROR (Status) && AutoStop) {
      // An open-ended multi-block transfer is still running in the card
      DwMmcHcAbortTransfer (DevBase);
    }

    // SCR comes most significant byte first; SD_SCR is laid out as a 64-bit
    // little endian number.
    if (!EFI_ERROR (Status) && (Packet->SdMmcCmdBlk->CommandIndex == SD_SEND_SCR) &&
        (Packet->SdMmcCmdBlk->CommandType == SdMmcCommandTypeAdtc) && (Trb->DataLen == 8))
    {
      UINT8  *Scr = Trb->Data;
      UINT8  Tmp;
      UINTN  I;
      for (I = 0; I < 4; I++) {
        Tmp         = Scr[I];
        Scr[I]      = Scr[7 - I];
        Scr[7 - I]  = Tmp;
      }
    }
  }

Done:
  if (Trb->DataLen != 0) {
    DwMmcHcStopDma (Private);
  }

  Trb->Result = Status;
  return Status;
}

/**
  Check the TRB execution result. DwMmcExecTrb has run the command to the end
  so there is nothing left to wait for.

  @retval The result of the command.

**/
EFI_STATUS
DwMmcCheckTrbResult (
  IN DW_MMC_HC_PRIVATE_DATA  *Private,
  IN DW_MMC_HC_TRB           *Trb
  )
{
  return Trb->Result;
}

/**
  Wait for the TRB execution result.

  @retval The result of the command.

**/
EFI_STATUS
DwMmcWaitTrbResult (
  IN DW_MMC_HC_PRIVATE_DATA  *Private,
  IN DW_MMC_HC_TRB           *Trb
  )
{
  return DwMmcCheckTrbResult (Private, Trb);
}

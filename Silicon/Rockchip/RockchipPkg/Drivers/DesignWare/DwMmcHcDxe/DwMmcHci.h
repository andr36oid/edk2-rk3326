/** @file

  Register-level helpers of the DesignWare SD/MMC host controller driver.

  Copyright (c) 2015-2021, Intel Corporation. All rights reserved.
  Copyright (c) 2018, Linaro Ltd. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _DW_MMC_HCI_H_
#define _DW_MMC_HCI_H_

#include <Library/CacheMaintenanceLib.h>
#include <Library/TimerLib.h>

#include <Protocol/PlatformDwMmc.h>

#include "DwMmcRegs.h"
#include "DwMmcMath.h"

// One slot per controller on PX30
#define DW_MMC_HC_MAX_SLOT  1

// Identification clock, kHz
#define DWMMC_INIT_CLOCK_FREQ  400

/**
  Print the slot capabilities the platform driver reported.

  @param[in]  Slot            The slot number.
  @param[in]  Capability      The capabilities.
**/
VOID
DumpCapabilityReg (
  IN UINT8               Slot,
  IN DW_MMC_HC_SLOT_CAP  *Capability
  );

/**
  Ask the platform driver for the slot capabilities.
**/
EFI_STATUS
DwMmcHcGetCapability (
  IN     UINTN               DevBase,
  IN     EFI_HANDLE          Controller,
  IN     UINT8               Slot,
  OUT DW_MMC_HC_SLOT_CAP     *Capacity
  );

/**
  Ask the platform driver whether a card is in the slot.
**/
EFI_STATUS
DwMmcHcCardDetect (
  IN     UINTN       DevBase,
  IN     EFI_HANDLE  Controller,
  IN     UINT8       Slot,
  OUT BOOLEAN        *MediaPresent
  );

/**
  Stop the card clock.

  @retval EFI_SUCCESS       Stopped.
  @retval EFI_TIMEOUT       The controller did not take the clock update.
  @retval EFI_DEVICE_ERROR  Hardware lock error on the clock update.
**/
EFI_STATUS
DwMmcHcStopClock (
  IN UINTN  DevBase
  );

/**
  Set the card clock and turn it on.

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
  );

/**
  Set the data bus width and single or double data rate.

  @param[in] DevBase        Controller registers.
  @param[in] IsDdr          Dual data rate.
  @param[in] BusWidth       1, 4 or 8.
**/
EFI_STATUS
DwMmcHcSetBusWidth (
  IN UINTN    DevBase,
  IN BOOLEAN  IsDdr,
  IN UINT16   BusWidth
  );

/**
  Start the card clock at the identification frequency.
**/
EFI_STATUS
DwMmcHcInitClockFreq (
  IN UINTN               DevBase,
  IN DW_MMC_HC_SLOT_CAP  Capability
  );

/**
  Program the timeouts and the card detect debounce.
**/
EFI_STATUS
DwMmcHcInitTimeoutCtrl (
  IN UINTN  DevBase
  );

/**
  Bring the card side of the controller to its identification state:
  interrupts cleared, timeouts set, block size 512, 400 kHz clock, 1-bit bus.
**/
EFI_STATUS
DwMmcHcReset (
  IN UINTN               DevBase,
  IN DW_MMC_HC_SLOT_CAP  Capability
  );

/**
  Clear and mask the controller interrupts. The driver polls.
**/
EFI_STATUS
DwMmcHcEnableInterrupt (
  IN UINTN  DevBase
  );

#endif

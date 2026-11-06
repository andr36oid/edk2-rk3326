/** @file
  The SoC side of the USB host port: everything between reset and the first
  DWC2 register access (power domain, clocks, reset, PHY), and the 5 V supply.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef RK_USB_PLATFORM_LIB_H_
#define RK_USB_PLATFORM_LIB_H_

#include <Uefi.h>

/**
  Bring the OTG controller up far enough that its registers answer, with the
  PHY out of suspend and forced to the host role. Does not touch the 5 V
  supply (RkUsbPlatformApplyFdt does). Safe to call again.

  @param[out] Base   Register base of the DWC2 core.

  @retval EFI_SUCCESS       The core answers (Synopsys ID read back).
  @retval EFI_DEVICE_ERROR  A bounded wait ran out or the ID is wrong.
**/
EFI_STATUS
EFIAPI
RkUsbPlatformPowerOn (
  OUT UINT64  *Base
  );

/**
  Decide and apply the port's 5 V supply from the board device tree: the
  vcc_host GPIO, the PMIC boost for boards where it only runs for an OTG
  device, and the charge mode on boards that share the port with the charger.

  @param[in] Fdt  The Android DTB of this board, NULL if there is none
                  (nothing is switched then, the PHY is still host).

  @retval EFI_SUCCESS    Host power is on (or there is nothing to switch).
  @retval EFI_NOT_READY  Charge mode: a charger is in on a board that shares
                         the port, so the port was left charging.
**/
EFI_STATUS
EFIAPI
RkUsbPlatformApplyFdt (
  IN CONST VOID  *Fdt OPTIONAL
  );

#endif

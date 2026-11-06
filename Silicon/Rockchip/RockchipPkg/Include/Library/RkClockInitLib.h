/** @file
  The PLLs and bus clocks U-Boot sets before Linux: APLL (ARMCLK) 600 MHz,
  GPLL 1200 MHz and the ACLK/HCLK/PCLK bus and peripheral pre-dividers.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _RK_CLOCK_INIT_LIB_H_
#define _RK_CLOCK_INIT_LIB_H_

#include <Uefi.h>

/**
  Bring the clocks to U-Boot's values. A PLL already at its target is left
  alone unless Force is TRUE (then it is reprogrammed). Dividers are written
  only where they differ.

  @retval EFI_SUCCESS       Everything is at its target.
  @retval EFI_DEVICE_ERROR  A PLL did not lock; it is left on the 24 MHz crystal.
**/
EFI_STATUS
EFIAPI
RkClockInit (
  IN BOOLEAN  Force
  );

#endif

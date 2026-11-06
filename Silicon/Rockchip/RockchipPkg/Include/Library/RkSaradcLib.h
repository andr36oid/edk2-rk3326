/** @file
  RK3326 SARADC, single conversions (10 bit, 3 channels).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _RK_SARADC_LIB_H_
#define _RK_SARADC_LIB_H_

#include <Uefi.h>

/**
  One conversion on Channel (0..2). Powers the ADC up for it and down again.
  Value is the raw 10 bit code. The reference is the SoC's 1.8 V rail
  (RK817 LDO2), which is on from power-on.
**/
EFI_STATUS
EFIAPI
RkSaradcRead (
  IN  UINT8   Channel,
  OUT UINT32  *Value
  );

#endif

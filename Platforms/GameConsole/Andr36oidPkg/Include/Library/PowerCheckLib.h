/** @file
  The power checks U-Boot makes before it boots anything: low battery without
  a charger (shut down), charger in at a cold start (charge screen until the
  power key is held). Call once at the start of the boot app.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _POWER_CHECK_LIB_H_
#define _POWER_CHECK_LIB_H_

#include <Uefi.h>

typedef enum {
  PowerCheckContinue,    // carry on booting
  PowerCheckPowerOff     // a shutdown was asked for and did not happen
} POWER_CHECK_RESULT;

/**
  Run the checks. When they decide to power off, this shuts the console down
  and does not return unless the shutdown fails. Skipped (returns Continue)
  when U-Boot ran first: it did the same checks.
**/
POWER_CHECK_RESULT
EFIAPI
PowerCheck (
  VOID
  );

#endif

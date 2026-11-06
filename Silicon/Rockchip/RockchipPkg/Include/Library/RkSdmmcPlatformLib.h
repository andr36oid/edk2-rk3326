/** @file
 *
 *  RkSdmmcDxe platform helper library.
 *
 *  Copyright (c) 2023, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

typedef enum {
  RkSdmmcCardPresenceUnsupported = 0,
  RkSdmmcCardPresent,
  RkSdmmcCardNotPresent
} RKSDMMC_CARD_PRESENCE_STATE;

EFI_STATUS
EFIAPI
RkSdmmcSetClockRate (
  IN UINTN  Frequency
  );

VOID
EFIAPI
RkSdmmcSetIoMux (
  VOID
  );

/**
  Hand the eMMC its pins (on an SD start they may be button pins).
**/
VOID
EFIAPI
RkSdmmcSetEmmcIoMux (
  VOID
  );

RKSDMMC_CARD_PRESENCE_STATE
EFIAPI
RkSdmmcGetCardPresenceState (
  VOID
  );

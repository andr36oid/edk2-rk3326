/** @file
  Buttons the boot logic looks at, sampled directly (no key queue): what
  boot.ini checks with "gpio input" at power-on, plus the menu keys.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _BOOT_KEYS_LIB_H_
#define _BOOT_KEYS_LIB_H_

#include <Uefi.h>

typedef enum {
  BootKeyFn,          // recovery when held at power-on
  BootKeySelect,      // boot log
  BootKeyStart,       // with Select: safe start
  BootKeyUp,
  BootKeyDown,
  BootKeyLeft,
  BootKeyRight,
  BootKeyA,           // confirm
  BootKeyB,           // back
  BootKeyL1,
  BootKeyR1,
  BootKeyMax
} BOOT_KEY;

typedef enum {
  BoardFamilyUnknown,
  BoardFamilyRg351mp,   // R36S and clones wired like it (SARADC ch0 146..186)
  BoardFamilyRg351v,    // EE clones: K36, G80, V12, R36 Max, ... (494..534)
  BoardFamilyRg351p     // (655..695)
} BOARD_FAMILY;

/**
  Board family from the board-id resistor (same thresholds as U-Boot's hwrev).
**/
BOARD_FAMILY
EFIAPI
BootKeysGetBoardFamily (
  VOID
  );

/**
  TRUE while Key is held. FALSE for keys the board doesn't have or the
  family doesn't map (like boot.ini, which only checks known families).
**/
BOOLEAN
EFIAPI
BootKeyPressed (
  IN BOOT_KEY  Key
  );

#endif

/** @file
  Buttons and board family for the RK3326 consoles, read straight from the
  pads and the SARADC (nothing depends on what U-Boot left behind).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/BootKeysLib.h>
#include <Library/CruLib.h>
#include <Library/GpioLib.h>
#include <Library/RkSaradcLib.h>
#include "BootKeysMap.h"

STATIC BOOLEAN       mReady;
STATIC BOARD_FAMILY  mFamily = BoardFamilyUnknown;

STATIC CONST CHAR8  *mKeyNames[BootKeyMax] = {
  [BootKeyFn]     = "Fn",
  [BootKeySelect] = "Select",
  [BootKeyStart]  = "Start",
  [BootKeyUp]     = "Up",
  [BootKeyDown]   = "Down",
  [BootKeyLeft]   = "Left",
  [BootKeyRight]  = "Right",
  [BootKeyA]      = "A",
  [BootKeyB]      = "B",
  [BootKeyL1]     = "L1",
  [BootKeyR1]     = "R1",
};

STATIC CONST CHAR8 *
FamilyName (
  IN BOARD_FAMILY  Family
  )
{
  switch (Family) {
    case BoardFamilyRg351mp:
      return "rg351mp";
    case BoardFamilyRg351v:
      return "rg351v";
    case BoardFamilyRg351p:
      return "rg351p";
    default:
      return "unknown (v00)";
  }
}

STATIC
VOID
KeysInit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Adc;
  UINTN       Key;
  KEY_PIN     Pin;

  if (mReady) {
    return;
  }

  mReady = TRUE;

  Adc = 0;
  Status = RkSaradcRead (0, &Adc);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootKeys: SARADC ch0 read failed: %r\n", Status));
    mFamily = BoardFamilyUnknown;
  } else {
    mFamily = BoardFamilyFromAdc (Adc);
  }

  DEBUG ((DEBUG_INFO, "BootKeys: SARADC ch0 = %u, board family %a\n", Adc, FamilyName (mFamily)));

  //
  // GPIO function, input, pull-up. boot.ini sets the pull-ups of the keys it
  // reads itself because U-Boot only does it on some boards.
  //
  CruEnableGpioClocks ();
  for (Key = 0; Key < BootKeyMax; Key++) {
    if (!BootKeyPin (mFamily, (BOOT_KEY)Key, &Pin)) {
      continue;
    }

    GpioPinSetFunction (Pin.Group, Pin.Pin, 0);
    GpioPinSetDirection (Pin.Group, Pin.Pin, GPIO_PIN_INPUT);
    GpioPinSetPull (Pin.Group, Pin.Pin, GPIO_PIN_PULL_UP);
  }

  for (Key = 0; Key < BootKeyMax; Key++) {
    if (BootKeyPin (mFamily, (BOOT_KEY)Key, &Pin)) {
      DEBUG ((
        DEBUG_INFO,
        "BootKeys: %a = GPIO%u pin %u, %a\n",
        mKeyNames[Key],
        Pin.Group,
        Pin.Pin,
        GpioPinGetLevel (Pin.Group, Pin.Pin) ? "up" : "HELD"
        ));
    }
  }
}

BOARD_FAMILY
EFIAPI
BootKeysGetBoardFamily (
  VOID
  )
{
  KeysInit ();
  return mFamily;
}

BOOLEAN
EFIAPI
BootKeyPressed (
  IN BOOT_KEY  Key
  )
{
  KEY_PIN  Pin;

  KeysInit ();
  if (!BootKeyPin (mFamily, Key, &Pin)) {
    return FALSE;
  }

  return !GpioPinGetLevel (Pin.Group, Pin.Pin);
}

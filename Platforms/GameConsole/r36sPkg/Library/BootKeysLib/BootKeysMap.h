/** @file
  Which pin is which button, per board family. Plain data and arithmetic, no
  hardware access, so a host test can check it against the kernel DTS and
  boot.ini. The includer provides Library/BootKeysLib.h and Library/GpioLib.h.

  All buttons are active low (pad pulled up, a press connects it to ground).

  Sources:
    rg351mp  kernel rk3326-r36s-android.dtsi (odroidgo3-joypad), boot.ini-android
    rg351v   kernel rk3326-r36s-clone-ee.dtsi (&joypad), boot.ini-android
    rg351p   no verified button map; the menu keys use the rg351mp one

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _BOOT_KEYS_MAP_H_
#define _BOOT_KEYS_MAP_H_

#define KEY_PIN_NONE  0xFF

typedef struct {
  UINT8  Group;     // GPIO bank 0..3, KEY_PIN_NONE if the key has no pin
  UINT8  Pin;       // GPIO_PIN_*
} KEY_PIN;

//
// SARADC channel 0 reads a resistor that tells the board layout. Same ranges
// as U-Boot's hwrev command; the limits themselves are not part of a range.
//
STATIC
BOARD_FAMILY
BoardFamilyFromAdc (
  IN UINT32  Adc
  )
{
  if ((Adc > 146) && (Adc < 186)) {
    return BoardFamilyRg351mp;
  }

  if ((Adc > 494) && (Adc < 534)) {
    return BoardFamilyRg351v;
  }

  if ((Adc > 655) && (Adc < 695)) {
    return BoardFamilyRg351p;
  }

  return BoardFamilyUnknown;
}

STATIC
CONST KEY_PIN *
BootKeyTable (
  IN BOARD_FAMILY  Family
  )
{
  STATIC CONST KEY_PIN  Rg351mp[BootKeyMax] = {
    [BootKeyFn]     = { 2, GPIO_PIN_PA4 },
    [BootKeySelect] = { 3, GPIO_PIN_PB1 },
    [BootKeyStart]  = { 3, GPIO_PIN_PB4 },
    [BootKeyUp]     = { 1, GPIO_PIN_PB4 },
    [BootKeyDown]   = { 1, GPIO_PIN_PB5 },
    [BootKeyLeft]   = { 1, GPIO_PIN_PB6 },
    [BootKeyRight]  = { 1, GPIO_PIN_PB7 },
    [BootKeyA]      = { 1, GPIO_PIN_PA2 },
    [BootKeyB]      = { 1, GPIO_PIN_PA5 },
    [BootKeyL1]     = { 2, GPIO_PIN_PA6 },
    [BootKeyR1]     = { 2, GPIO_PIN_PA7 },
  };
  STATIC CONST KEY_PIN  Rg351v[BootKeyMax] = {
    [BootKeyFn]     = { 3, GPIO_PIN_PB2 },
    [BootKeySelect] = { 3, GPIO_PIN_PD0 },
    [BootKeyStart]  = { 3, GPIO_PIN_PD1 },
    [BootKeyUp]     = { 3, GPIO_PIN_PC6 },
    [BootKeyDown]   = { 3, GPIO_PIN_PC7 },
    [BootKeyLeft]   = { 3, GPIO_PIN_PC4 },
    [BootKeyRight]  = { 3, GPIO_PIN_PC5 },
    [BootKeyA]      = { 3, GPIO_PIN_PC0 },
    [BootKeyB]      = { 3, GPIO_PIN_PC1 },
    [BootKeyL1]     = { 3, GPIO_PIN_PB5 },
    [BootKeyR1]     = { 3, GPIO_PIN_PB3 },
  };

  return (Family == BoardFamilyRg351v) ? Rg351v : Rg351mp;
}

//
// FN, Select and Start decide how to boot, so they are only read where
// boot.ini reads them. Menu navigation keys also work on the other boards on
// the assumption that they are wired like an R36S.
//
STATIC
BOOLEAN
BootKeyPin (
  IN  BOARD_FAMILY  Family,
  IN  BOOT_KEY      Key,
  OUT KEY_PIN       *Pin
  )
{
  CONST KEY_PIN  *Entry;

  if ((UINT32)Key >= BootKeyMax) {
    return FALSE;
  }

  if ((Family != BoardFamilyRg351mp) && (Family != BoardFamilyRg351v) &&
      ((Key == BootKeyFn) || (Key == BootKeySelect) || (Key == BootKeyStart)))
  {
    return FALSE;
  }

  Entry = &BootKeyTable (Family)[Key];

  //
  // A table slot left out is all zero. GPIO0 has no buttons, so group 0 means
  // "no pin".
  //
  if ((Entry->Group == 0) || (Entry->Group == KEY_PIN_NONE)) {
    return FALSE;
  }

  *Pin = *Entry;
  return TRUE;
}

#endif

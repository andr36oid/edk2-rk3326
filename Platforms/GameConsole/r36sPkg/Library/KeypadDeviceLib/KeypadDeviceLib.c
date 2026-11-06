/** @file
  KeypadDeviceLib for SiliciumPkg's keypad driver: the d-pad and A/B buttons
  drive the UEFI text menus.

    Up/Down/Left/Right  arrow keys (repeat when held)
    A                   Enter
    B                   Escape
    L1 / R1             Page Up / Page Down (repeat when held)

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/BootKeysLib.h>
#include <Library/KeypadDeviceLib.h>
#include "KeyRepeat.h"

typedef struct {
  BOOT_KEY    Key;
  UINT16      ScanCode;
  CHAR16      UnicodeChar;
  BOOLEAN     Repeats;
  KEY_REPEAT  State;
} KEY_MAP;

STATIC KEY_MAP  mKeys[] = {
  { BootKeyUp,    SCAN_UP,        0,                    TRUE  },
  { BootKeyDown,  SCAN_DOWN,      0,                    TRUE  },
  { BootKeyLeft,  SCAN_LEFT,      0,                    TRUE  },
  { BootKeyRight, SCAN_RIGHT,     0,                    TRUE  },
  { BootKeyA,     SCAN_NULL,      CHAR_CARRIAGE_RETURN, FALSE },
  { BootKeyB,     SCAN_ESC,       0,                    FALSE },
  { BootKeyL1,    SCAN_PAGE_UP,   0,                    TRUE  },
  { BootKeyR1,    SCAN_PAGE_DOWN, 0,                    TRUE  },
};

EFI_STATUS
EFIAPI
KeypadDeviceReset (
  KEYPAD_DEVICE_PROTOCOL  *This
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mKeys); Index++) {
    mKeys[Index].State.Down      = FALSE;
    mKeys[Index].State.Repeating = FALSE;
    mKeys[Index].State.HeldNs    = 0;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
KeypadDeviceGetKeys (
  KEYPAD_DEVICE_PROTOCOL  *This,
  KEYPAD_RETURN_API       *KeypadReturnApi,
  UINT64                  Delta
  )
{
  UINTN         Index;
  UINT32        Events;
  EFI_KEY_DATA  KeyData;

  for (Index = 0; Index < ARRAY_SIZE (mKeys); Index++) {
    Events = KeyRepeatStep (
               &mKeys[Index].State,
               BootKeyPressed (mKeys[Index].Key),
               Delta,
               mKeys[Index].Repeats
               );

    ZeroMem (&KeyData, sizeof (KeyData));
    KeyData.Key.ScanCode    = mKeys[Index].ScanCode;
    KeyData.Key.UnicodeChar = mKeys[Index].UnicodeChar;
    while (Events-- > 0) {
      KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &KeyData);
    }
  }

  return EFI_SUCCESS;
}

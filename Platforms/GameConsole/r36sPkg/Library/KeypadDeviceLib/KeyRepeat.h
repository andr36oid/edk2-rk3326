/** @file
  Press and auto-repeat timing of one button. No hardware access.
  The includer provides UINT8/UINT64/BOOLEAN.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _KEY_REPEAT_H_
#define _KEY_REPEAT_H_

#define KEY_REPEAT_DELAY_NS     500000000ULL
#define KEY_REPEAT_INTERVAL_NS  100000000ULL

typedef struct {
  BOOLEAN  Down;
  UINT64   HeldNs;      // time since the press or the last repeat
  BOOLEAN  Repeating;
} KEY_REPEAT;

//
// Feed the current button level and the time since the last call. Returns how
// many key events to report: 1 on the press, then one per repeat interval
// after the repeat delay (only if the key repeats).
//
STATIC
UINT32
KeyRepeatStep (
  IN OUT KEY_REPEAT  *K,
  IN     BOOLEAN     Pressed,
  IN     UINT64      DeltaNs,
  IN     BOOLEAN     Repeats
  )
{
  UINT32  Events;

  if (!Pressed) {
    K->Down      = FALSE;
    K->Repeating = FALSE;
    K->HeldNs    = 0;
    return 0;
  }

  if (!K->Down) {
    K->Down      = TRUE;
    K->Repeating = FALSE;
    K->HeldNs    = 0;
    return 1;
  }

  if (!Repeats) {
    return 0;
  }

  Events     = 0;
  K->HeldNs += DeltaNs;
  if (!K->Repeating) {
    if (K->HeldNs >= KEY_REPEAT_DELAY_NS) {
      K->HeldNs   -= KEY_REPEAT_DELAY_NS;
      K->Repeating = TRUE;
      Events++;
    }
  }

  if (K->Repeating) {
    while (K->HeldNs >= KEY_REPEAT_INTERVAL_NS) {
      K->HeldNs -= KEY_REPEAT_INTERVAL_NS;
      Events++;
    }
  }

  return Events;
}

#endif

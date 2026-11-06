/** @file
  Text boot menu on the console (GOP text console on the handhelds, the
  serial console on the QEMU test device).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Andr36oidBoot.h"

#include <Library/BaseLib.h>
#include <Library/BootKeysLib.h>
#include <Library/DebugLib.h>
#include <Library/RkPmicLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define POLL_US           20000
#define IDLE_POWEROFF_US  (5ULL * 60 * 1000 * 1000)   // nobody there: don't drain the battery

typedef enum {
  ItemAndroid,
  ItemBootLog,
  ItemSafe,
  ItemRecovery,
  ItemSaveLog,
  ItemPowerOff,
  ItemCount
} MENU_ITEM;

STATIC CONST CHAR16  *mItems[ItemCount] = {
  L"Start Android",
  L"Start Android with the boot log on screen",
  L"Safe start (no Kernel Tweaks, no Magisk)",
  L"Recovery",
  L"Save the log to BOOT (uefi-log.txt)",
  L"Power off"
};

STATIC CONST BOOT_CHOICE  mChoice[ItemCount] = {
  BootChoiceAndroid,
  BootChoiceBootLog,
  BootChoiceSafe,
  BootChoiceRecovery,
  BootChoiceNone,
  BootChoiceNone
};

typedef enum {
  InputNone,
  InputUp,
  InputDown,
  InputSelect,
  InputItem       // a number key: Item holds which
} MENU_INPUT;

VOID
BootPowerOff (
  VOID
  )
{
  Say ("Powering off\n");
  RkPmicPowerOff ();
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();
}

STATIC
VOID
Draw (
  IN UINTN        Selected,
  IN CONST CHAR8  *Message OPTIONAL,
  IN CONST CHAR8  *Status OPTIONAL
  )
{
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *Out = gST->ConOut;
  UINTN                            I;

  Out->SetAttribute (Out, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
  Out->ClearScreen (Out);
  Out->EnableCursor (Out, FALSE);
  Print (L"\n  andr36oid boot menu\n\n");
  if (Message != NULL) {
    Out->SetAttribute (Out, EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK));
    Print (L"  %a\n\n", Message);
  }

  for (I = 0; I < ItemCount; I++) {
    Out->SetAttribute (Out, (I == Selected) ? EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY) : EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
    Print (L"  %d. %s  \n", I + 1, mItems[I]);
  }

  Out->SetAttribute (Out, EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK));
  Print (L"\n  Up/Down to choose, A or Enter to start\n");
  if (Status != NULL) {
    Out->SetAttribute (Out, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK));
    Print (L"\n  %a\n", Status);
  }

  Out->SetAttribute (Out, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
}

//
// One input. Boards whose buttons BootKeysLib reads: the d-pad and A from
// there (press edges), and the keypad driver's copies of them on ConIn
// (arrows, Enter, Esc) are dropped so a press counts once. Number keys and,
// on boards without buttons (QEMU), every key come from ConIn.
//
STATIC
MENU_INPUT
ReadInput (
  IN OUT BOOLEAN  *Held,
  OUT    UINTN    *Item
  )
{
  STATIC CONST BOOT_KEY  Keys[] = { BootKeyUp, BootKeyDown, BootKeyA };
  STATIC CONST MENU_INPUT  Map[] = { InputUp, InputDown, InputSelect };
  EFI_INPUT_KEY            Key;
  BOOLEAN                  Buttons = (BootKeysGetBoardFamily () != BoardFamilyUnknown);
  BOOLEAN                  Now;
  MENU_INPUT               Result = InputNone;
  UINTN                    I;

  if (Buttons) {
    for (I = 0; I < ARRAY_SIZE (Keys); I++) {
      Now = BootKeyPressed (Keys[I]);
      if (Now && !Held[I] && (Result == InputNone)) {
        Result = Map[I];
      }

      Held[I] = Now;
    }

    if (Result != InputNone) {
      return Result;
    }
  }

  while (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key) == EFI_SUCCESS) {
    if ((Key.UnicodeChar >= L'1') && (Key.UnicodeChar < L'1' + ItemCount)) {
      *Item = Key.UnicodeChar - L'1';
      return InputItem;
    }

    if (Buttons) {
      continue;
    }

    if ((Key.ScanCode == SCAN_UP) || (Key.UnicodeChar == L'k')) {
      return InputUp;
    }

    if ((Key.ScanCode == SCAN_DOWN) || (Key.UnicodeChar == L'j')) {
      return InputDown;
    }

    if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key.UnicodeChar == CHAR_LINEFEED)) {
      return InputSelect;
    }
  }

  return InputNone;
}

BOOT_CHOICE
BootMenu (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR8        *Message OPTIONAL
  )
{
  BOOLEAN      Held[3];
  UINTN        Selected = ItemAndroid;
  UINTN        Item;
  UINT64       Idle     = 0;
  CONST CHAR8  *Status  = NULL;
  MENU_INPUT   In;
  UINTN        I;

  // Keys held when the menu opens (Select, A) only count once let go.
  for (I = 0; I < ARRAY_SIZE (Held); I++) {
    Held[I] = TRUE;
  }

  Say ("boot: menu%a%a\n", (Message != NULL) ? ": " : "", (Message != NULL) ? Message : "");
  Draw (Selected, Message, NULL);
  for ( ; ;) {
    In = ReadInput (Held, &Item);
    if (In == InputNone) {
      gBS->Stall (POLL_US);
      Idle += POLL_US;
      if (Idle >= IDLE_POWEROFF_US) {
        Say ("boot: menu idle for 5 minutes\n");
        BootPowerOff ();
      }

      continue;
    }

    Idle = 0;
    switch (In) {
      case InputUp:
        Selected = (Selected + ItemCount - 1) % ItemCount;
        Draw (Selected, Message, Status);
        continue;
      case InputDown:
        Selected = (Selected + 1) % ItemCount;
        Draw (Selected, Message, Status);
        continue;
      case InputItem:
        Selected = Item;
        break;
      default:
        break;
    }

    Say ("boot: menu: %s\n", mItems[Selected]);
    switch (Selected) {
      case ItemSaveLog:
        Status = EFI_ERROR (BootSaveLogs (Root)) ? "Could not write the log to BOOT" : "Log saved to BOOT/uefi-log.txt";
        Draw (Selected, Message, Status);
        break;
      case ItemPowerOff:
        BootSaveLogs (Root);
        BootPowerOff ();
        break;
      default:
        gST->ConOut->ClearScreen (gST->ConOut);
        return mChoice[Selected];
    }
  }
}

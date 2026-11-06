/** @file
  Text boot menu on the console (GOP text console on the handhelds, the
  serial console on the QEMU test device).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Andr36oidBoot.h"

#include <Library/BaseLib.h>
#include <Library/BootKeysLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/RkPmicLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/EfiKeypadDevice.h>

#define POLL_US           20000
#define IDLE_POWEROFF_US  (5ULL * 60 * 1000 * 1000)   // nobody there: don't drain the battery

typedef enum {
  ItemAndroid,
  ItemBootLog,
  ItemSafe,
  ItemRecovery,
  ItemUsb,          // only while a USB stick with BOOT files is in
  ItemSaveLog,
  ItemPowerOff,
  ItemCount
} MENU_ITEM;

STATIC CONST CHAR16  *mItems[ItemCount] = {
  L"Start Android",
  L"Start Android with the boot log on screen",
  L"Safe start (no Kernel Tweaks, no Magisk)",
  L"Recovery",
  L"Start from the USB stick",
  L"Save the log to BOOT (uefi-log.txt)",
  L"Power off"
};

STATIC CONST BOOT_CHOICE  mChoice[ItemCount] = {
  BootChoiceAndroid,
  BootChoiceBootLog,
  BootChoiceSafe,
  BootChoiceRecovery,
  BootChoiceAndroid,
  BootChoiceNone,
  BootChoiceNone
};

STATIC BOOLEAN  mUsb;     // ItemUsb is shown

typedef enum {
  InputNone,
  InputUp,
  InputDown,
  InputSelect,
  InputItem       // a number key: Item holds which
} MENU_INPUT;

STATIC
BOOLEAN
Shown (
  IN UINTN  Item
  )
{
  return (Item != ItemUsb) || mUsb;
}

//
// The item with that number on screen (1 = first shown), ItemCount if none.
//
STATIC
UINTN
ItemNumbered (
  IN UINTN  Number
  )
{
  UINTN  I;

  for (I = 0; I < ItemCount; I++) {
    if (Shown (I) && (--Number == 0)) {
      return I;
    }
  }

  return ItemCount;
}

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
  UINTN                            N;

  Out->SetAttribute (Out, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
  Out->ClearScreen (Out);
  Out->EnableCursor (Out, FALSE);
  Print (L"\n  andr36oid boot menu\n\n");
  if (Message != NULL) {
    Out->SetAttribute (Out, EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK));
    Print (L"  %a\n\n", Message);
  }

  for (I = 0, N = 0; I < ItemCount; I++) {
    if (!Shown (I)) {
      continue;
    }

    Out->SetAttribute (Out, (I == Selected) ? EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY) : EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
    Print (L"  %d. %s  \n", ++N, mItems[I]);
  }

  Out->SetAttribute (Out, EFI_TEXT_ATTR (EFI_DARKGRAY, EFI_BLACK));
  Print (L"\n  Up/Down to choose, A or Enter to start\n");
  if (Status != NULL) {
    Out->SetAttribute (Out, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK));
    Print (L"\n  %a\n", Status);
  }

  Out->SetAttribute (Out, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
}

STATIC
MENU_INPUT
KeyInput (
  IN  CONST EFI_INPUT_KEY  *Key,
  OUT UINTN                *Item
  )
{
  if ((Key->UnicodeChar >= L'1') && (Key->UnicodeChar <= L'9') && (ItemNumbered (Key->UnicodeChar - L'0') < ItemCount)) {
    *Item = ItemNumbered (Key->UnicodeChar - L'0');
    return InputItem;
  }

  if ((Key->ScanCode == SCAN_UP) || (Key->UnicodeChar == L'k')) {
    return InputUp;
  }

  if ((Key->ScanCode == SCAN_DOWN) || (Key->UnicodeChar == L'j')) {
    return InputDown;
  }

  if ((Key->UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key->UnicodeChar == CHAR_LINEFEED)) {
    return InputSelect;
  }

  return InputNone;
}

//
// One input. Boards whose buttons BootKeysLib reads: the d-pad and A from
// there (press edges), and the keypad driver's copies of them (arrows,
// Enter, Esc on its own text input) are dropped so a press counts once.
// Every other keyboard (USB, the serial console) is read on its own, not
// through ConIn, so its arrows and Enter work on every board.
//
STATIC
MENU_INPUT
ReadInput (
  IN OUT BOOLEAN  *Held,
  OUT    UINTN    *Item
  )
{
  STATIC CONST BOOT_KEY    Keys[] = { BootKeyUp, BootKeyDown, BootKeyA };
  STATIC CONST MENU_INPUT  Map[]  = { InputUp, InputDown, InputSelect };
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *In;
  EFI_INPUT_KEY                   Key;
  EFI_HANDLE                      *Handles;
  UINTN                           Count;
  BOOLEAN                         Buttons = (BootKeysGetBoardFamily () != BoardFamilyUnknown);
  BOOLEAN                         Keypad;
  BOOLEAN                         Now;
  MENU_INPUT                      Result = InputNone;
  VOID                            *Dummy;
  UINTN                           I;

  if (Buttons) {
    for (I = 0; I < ARRAY_SIZE (Keys); I++) {
      Now = BootKeyPressed (Keys[I]);
      if (Now && !Held[I] && (Result == InputNone)) {
        Result = Map[I];
      }

      Held[I] = Now;
    }
  }

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextInProtocolGuid, NULL, &Count, &Handles))) {
    return Result;
  }

  for (I = 0; I < Count; I++) {
    // ConSplitter's is all of them at once, the keypad's included.
    if ((Handles[I] == gST->ConsoleInHandle) ||
        EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiSimpleTextInProtocolGuid, (VOID **)&In)))
    {
      continue;
    }

    Keypad = Buttons && !EFI_ERROR (gBS->HandleProtocol (Handles[I], &gKeypadDeviceProtocolGuid, &Dummy));
    while ((Result == InputNone) && (In->ReadKeyStroke (In, &Key) == EFI_SUCCESS)) {
      if (!Keypad) {
        Result = KeyInput (&Key, Item);
      }
    }
  }

  FreePool (Handles);
  return Result;
}

//
// Show "Start from the USB stick" while one with BOOT files is in.
//
STATIC
BOOLEAN
CheckUsb (
  IN OUT UINTN  *Selected
  )
{
  EFI_FILE_PROTOCOL  *Usb = BootOpenVolume (BootDeviceUsb);
  BOOLEAN            Was  = mUsb;

  mUsb = (Usb != NULL);
  if (Usb != NULL) {
    Usb->Close (Usb);
  }

  if (!mUsb && (*Selected == ItemUsb)) {
    *Selected = ItemAndroid;
  }

  return mUsb != Was;
}

BOOT_CHOICE
BootMenu (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR8        *Message OPTIONAL,
  OUT BOOLEAN            *FromUsb
  )
{
  BOOLEAN      Held[3];
  UINTN        Selected = ItemAndroid;
  UINTN        Item;
  UINT64       Idle     = 0;
  UINTN        Polls    = 0;
  CONST CHAR8  *Status  = NULL;
  MENU_INPUT   In;
  UINTN        I;

  *FromUsb = FALSE;
  CheckUsb (&Selected);

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
      if ((++Polls % 50 == 0) && CheckUsb (&Selected)) {    // once a second
        Draw (Selected, Message, Status);
      }

      if (Idle >= IDLE_POWEROFF_US) {
        Say ("boot: menu idle for 5 minutes\n");
        BootPowerOff ();
      }

      continue;
    }

    Idle = 0;
    switch (In) {
      case InputUp:
        do {
          Selected = (Selected + ItemCount - 1) % ItemCount;
        } while (!Shown (Selected));

        Draw (Selected, Message, Status);
        continue;
      case InputDown:
        do {
          Selected = (Selected + 1) % ItemCount;
        } while (!Shown (Selected));

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
        *FromUsb = (Selected == ItemUsb);
        return mChoice[Selected];
    }
  }
}

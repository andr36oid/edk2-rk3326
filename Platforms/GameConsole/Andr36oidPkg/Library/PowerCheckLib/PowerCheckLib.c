/** @file
  Low-battery shutdown and the charge screen, after U-Boot's board_check_power
  (odroidgoa_power.c) and charge_animation.c. Drawing is the text console plus
  a few GOP rectangles for the battery, no images. The policy is in
  PowerPolicy.h so a host test can run it.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/CruLib.h>
#include <Library/GpioLib.h>
#include <Library/RkPmicLib.h>
#include <Library/PowerCheckLib.h>
#include <Guid/RkPlatformInfoHob.h>

#include "PowerPolicy.h"

#define DC_DET_BANK      0
#define DC_DET_PIN       GPIO_PIN_PB3       // GPIO0_B3, high = charger connected
#define BOOT_MODE_REG    (PMU_GRF + 0x200)  // ROCKCHIP_BOOT_MODE_REG, 0xFF010200
#define POLL_MS          50

typedef struct {
  RK_POWER_STATUS  Status;
  BOOLEAN          Ok;
} READING;

STATIC
VOID
Read (
  OUT READING  *R
  )
{
  R->Ok = !EFI_ERROR (RkPmicGetPowerStatus (&R->Status));
}

STATIC
VOID
PowerOffNow (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "PowerCheck: powering off\n"));
  RkPmicPowerOff ();
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  // Nothing worked; U-Boot loops here too.
  for ( ; ;) {
    MicroSecondDelay (100000);
  }
}

STATIC
VOID
Fill (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop,
  IN UINT32                        X,
  IN UINT32                        Y,
  IN UINT32                        W,
  IN UINT32                        H,
  IN UINT8                         R,
  IN UINT8                         G,
  IN UINT8                         B
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  P;

  P.Red = R; P.Green = G; P.Blue = B; P.Reserved = 0;
  Gop->Blt (Gop, &P, EfiBltVideoFill, 0, 0, X, Y, W, H, 0);
}

//
// A battery outline in the middle of the upper half, Percent of it filled
// (green, amber below 30 %, red below 10 %), plus the terminal nub.
//
STATIC
VOID
DrawBattery (
  IN UINT32   Percent,
  IN BOOLEAN  Charging
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  UINT32                        Sw;
  UINT32                        Sh;
  UINT32                        W;
  UINT32                        H;
  UINT32                        X;
  UINT32                        Y;
  UINT32                        T;
  UINT32                        Inner;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) || (Gop == NULL)) {
    return;
  }

  Sw = Gop->Mode->Info->HorizontalResolution;
  Sh = Gop->Mode->Info->VerticalResolution;
  W  = Sw / 3;
  H  = Sh / 6;
  X  = (Sw - W) / 2;
  Y  = Sh / 5;
  T  = (H / 12) + 1;

  Fill (Gop, X, Y, W, H, 255, 255, 255);
  Fill (Gop, X + T, Y + T, W - 2 * T, H - 2 * T, 0, 0, 0);
  Fill (Gop, X + W, Y + H / 3, T * 2, H / 3, 255, 255, 255);
  Inner = (W - 4 * T) * Percent / 100;
  if (Inner > 0) {
    if (Percent < 10) {
      Fill (Gop, X + 2 * T, Y + 2 * T, Inner, H - 4 * T, 220, 40, 40);
    } else if (Percent < 30) {
      Fill (Gop, X + 2 * T, Y + 2 * T, Inner, H - 4 * T, 230, 170, 30);
    } else {
      Fill (Gop, X + 2 * T, Y + 2 * T, Inner, H - 4 * T, 60, 200, 80);
    }
  }

  // A bar under the battery while it is charging.
  if (Charging) {
    Fill (Gop, X, Y + H + T, W, T, 60, 200, 80);
  }
}

STATIC
VOID
Line (
  IN UINTN         Row,
  IN CONST CHAR16  *Text
  )
{
  UINTN  Cols;
  UINTN  Rows;
  UINTN  Len;

  if (EFI_ERROR (gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, &Cols, &Rows))) {
    return;
  }

  Len = StrLen (Text);
  gST->ConOut->SetCursorPosition (gST->ConOut, (Len < Cols) ? (Cols - Len) / 2 : 0, (Row < Rows) ? Row : Rows - 1);
  gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Text);
}

STATIC
UINTN
TextRow (
  IN UINTN  Fraction
  )
{
  UINTN  Cols;
  UINTN  Rows;

  if (EFI_ERROR (gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, &Cols, &Rows))) {
    return 20;
  }

  return Rows * Fraction / 10;
}

STATIC
VOID
Black (
  VOID
  )
{
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK));
  gST->ConOut->ClearScreen (gST->ConOut);
}

STATIC
VOID
DrawLowBattery (
  IN UINT32  Mv
  )
{
  CHAR16  Text[64];

  Black ();
  DrawBattery (0, FALSE);
  Line (TextRow (5), L"LOW BATTERY");
  // U-Boot prints "voltage level : %d.%dV"; this keeps the leading zero of the decimals.
  UnicodeSPrint (Text, sizeof (Text), L"voltage level : %d.%03dV", Mv / 1000, Mv % 1000);
  Line (TextRow (6), Text);
  Line (TextRow (8), L"Connect a charger. Powering off.");
}

STATIC
VOID
DrawCharge (
  IN CONST POWER_CHARGE_STATE  *S,
  IN UINT32                    Mv,
  IN BOOLEAN                   Charging
  )
{
  CHAR16  Text[64];
  UINT32  Percent;

  Black ();
  if (!S->ScreenOn) {
    return;
  }

  Percent = PowerPercentFromMv (Mv);
  DrawBattery (Percent, Charging);
  if (S->Extreme) {
    Line (TextRow (5), L"Extreme low battery");
    Line (TextRow (6), L"Charging, the screen goes off soon");
  } else {
    UnicodeSPrint (Text, sizeof (Text), L"%a %d%%", Charging ? "Charging" : "Charged", Percent);
    Line (TextRow (5), Text);
    if (Mv != 0) {
      UnicodeSPrint (Text, sizeof (Text), L"%d.%03dV", Mv / 1000, Mv % 1000);
      Line (TextRow (6), Text);
    }

    Line (TextRow (8), L"Hold the power key to start");
  }
}

STATIC
VOID
LowBatteryScreen (
  IN UINT32  Mv
  )
{
  READING  R;
  UINT32   Waited;

  DrawLowBattery (Mv);
  // odroid_wait_pwrkey: up to 10 s for the key, then off either way.
  for (Waited = 0; Waited < POWER_LOW_BATT_WAIT_MS; Waited += POLL_MS) {
    MicroSecondDelay (POLL_MS * 1000);
    Read (&R);
    if (R.Ok && (R.Status.PowerKeyPressed || R.Status.PowerKeyTap)) {
      break;
    }
  }

  PowerOffNow ();
}

STATIC
POWER_CHECK_RESULT
ChargeScreen (
  VOID
  )
{
  POWER_CHARGE_STATE  S;
  POWER_CHARGE_OUT    Out;
  READING             R;
  UINT64              Now;
  UINT32              Mv;
  BOOLEAN             Charging;
  BOOLEAN             Plug;
  UINT32              Shown;
  UINT32              Errors;

  ZeroMem (&S, sizeof (S));
  Now      = 0;
  Mv       = 0;
  Charging = TRUE;
  Plug     = TRUE;
  Shown    = 0xFFFF;
  Errors   = 0;

  for ( ; ;) {
    Read (&R);
    if (R.Ok) {
      Errors = 0;
      Plug   = R.Status.ChargerPresent;
      if (R.Status.BatteryMicroVolts != 0) {
        Mv = R.Status.BatteryMicroVolts / 1000;
      }

      Charging = R.Status.Charging;
    } else if (++Errors > 40) {
      // Two seconds without an answer: stop guessing and boot.
      DEBUG ((DEBUG_ERROR, "PowerCheck: PMIC stopped answering, leaving the charge screen\n"));
      return PowerCheckContinue;
    }

    if (R.Ok) {
      PowerChargeStep (&S, Now, Mv, Plug, R.Status.PowerKeyPressed, R.Status.PowerKeyTap, &Out);
      if (Out.Action == PowerActPowerOff) {
        DEBUG ((DEBUG_INFO, "PowerCheck: charger removed\n"));
        PowerOffNow ();
      }

      if (Out.Action == PowerActContinueBoot) {
        DEBUG ((DEBUG_INFO, "PowerCheck: power key held, booting (%u mV)\n", Mv));
        Black ();
        return PowerCheckContinue;
      }

      // The numbers move slowly; redraw when the shown value changes too.
      if (S.ScreenOn && (Shown != (PowerPercentFromMv (Mv) * 2 + Charging))) {
        Out.Redraw = TRUE;
      }

      if (Out.Redraw) {
        Shown = PowerPercentFromMv (Mv) * 2 + Charging;
        DrawCharge (&S, Mv, Charging);
        DEBUG ((DEBUG_INFO, "PowerCheck: screen %a, %u mV, charging %d\n", S.ScreenOn ? "on" : "off", Mv, Charging));
      }
    }

    MicroSecondDelay (POLL_MS * 1000);
    Now += POLL_MS;
  }
}

POWER_CHECK_RESULT
EFIAPI
PowerCheck (
  VOID
  )
{
  EFI_HOB_GUID_TYPE           *Hob;
  CONST RK_PLATFORM_INFO_HOB  *Info;
  POWER_INPUTS                In;
  READING                     R;

  Hob = GetFirstGuidHob (&gRkPlatformInfoHobGuid);
  if (Hob != NULL) {
    Info = GET_GUID_HOB_DATA (Hob);
    if (Info->BootloaderFdt != 0) {
      DEBUG ((DEBUG_INFO, "PowerCheck: U-Boot ran first, its checks stand\n"));
      return PowerCheckContinue;
    }
  }

  CruEnableGpioClocks ();
  GpioPinSetDirection (DC_DET_BANK, DC_DET_PIN, GPIO_PIN_INPUT);

  ZeroMem (&In, sizeof (In));
  Read (&R);
  In.StatusOk = R.Ok;
  if (R.Ok) {
    In.BatteryMv      = R.Status.BatteryMicroVolts / 1000;
    In.BatteryPresent = R.Status.BatteryPresent;
    In.Plug           = R.Status.ChargerPresent;
    RkPmicSetChargerLimits ();
  }

  In.DcPin       = GpioPinGetLevel (DC_DET_BANK, DC_DET_PIN);
  In.BootModeReg = MmioRead32 (BOOT_MODE_REG);
  DEBUG ((
    DEBUG_INFO,
    "PowerCheck: pmic %d, %u mV, battery %d, dc pin %d, plug %d, boot mode reg 0x%X\n",
    In.StatusOk,
    In.BatteryMv,
    In.BatteryPresent,
    In.DcPin,
    In.Plug,
    In.BootModeReg
    ));

  switch (PowerStartDecide (&In)) {
    case PowerStartLowBattery:
      DEBUG ((DEBUG_INFO, "PowerCheck: low battery without a charger\n"));
      LowBatteryScreen (In.BatteryMv);
      break;
    case PowerStartCharge:
      DEBUG ((DEBUG_INFO, "PowerCheck: charger in, charge screen\n"));
      return ChargeScreen ();
    default:
      break;
  }

  return PowerCheckContinue;
}

/** @file
  What to do about the battery and charger, as plain functions of the readings
  (no hardware). The numbers are U-Boot's: odroidgoa_power.c (3530 mV),
  charge_animation.c and the screen DTB's charge-animation node (exit 3090 mV,
  low power 3055 mV + 50, screen off after 10 s), odroidgoa_status.c (10 s wait
  for the power key). The includer provides the fixed-width types and BOOLEAN.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _POWER_POLICY_H_
#define _POWER_POLICY_H_

#define POWER_MIN_BATTERY_MV      3530
#define POWER_EXTREME_LOW_MV      3105   // rockchip,uboot-low-power-voltage 3055 + 50
#define POWER_LOW_BATT_WAIT_MS    10000
#define POWER_SCREEN_OFF_MS       10000  // rockchip,auto-off-screen-interval
#define POWER_EXTREME_OFF_MS      5000
#define POWER_LONG_PRESS_MS       2000
#define POWER_BOOT_MODE_NORMAL    0x5242C300U   // BOOT_NORMAL in BOOT_MODE_REG (PMU_GRF 0x200)

typedef struct {
  BOOLEAN  StatusOk;        // the PMIC answered
  UINT32   BatteryMv;       // 0 if unknown
  BOOLEAN  BatteryPresent;
  BOOLEAN  DcPin;          // GPIO0_B3 high
  BOOLEAN  Plug;           // PMIC PLUG_IN_STS
  UINT32   BootModeReg;    // 0 after a cold start
} POWER_INPUTS;

typedef enum {
  PowerStartContinue,
  PowerStartLowBattery,
  PowerStartCharge
} POWER_START;

//
// board_check_power first (low battery without a charger), then the charge
// screen (charge_animation_show): needs the DC jack and the PMIC both saying a
// charger is in, a battery, and a boot mode that is not "normal" (a reboot from
// Linux writes NORMAL; a cold start leaves the register 0, which U-Boot's
// rockchip_get_boot_mode reads as undefined and so does show the screen).
// Deliberate difference: a PMIC plug-in also counts as a charger for the
// low-battery check, so a console that is plugged in is never shut down.
//
STATIC
POWER_START
PowerStartDecide (
  IN CONST POWER_INPUTS  *In
  )
{
  BOOLEAN  DcIn;

  if (!In->StatusOk) {
    return PowerStartContinue;
  }

  DcIn = In->DcPin || In->Plug;
  if ((In->BatteryMv != 0) && (In->BatteryMv < POWER_MIN_BATTERY_MV) && !DcIn) {
    return PowerStartLowBattery;
  }

  if (In->DcPin && In->Plug && In->BatteryPresent && (In->BootModeReg != POWER_BOOT_MODE_NORMAL)) {
    return PowerStartCharge;
  }

  return PowerStartContinue;
}

//
// Battery level for the charge screen, from the screen DTB's fuel gauge OCV
// table (battery node, 21 points 0..100 % in 5 % steps). While charging the
// terminal voltage is above the open-circuit one, so this reads high: it is an
// estimate, shown as bars, not a gauge.
//
STATIC
UINT32
PowerPercentFromMv (
  IN UINT32  Mv
  )
{
  STATIC CONST UINT16  Ocv[21] = {
    3165, 3220, 3280, 3330, 3380, 3430, 3480, 3530, 3580, 3630, 3670,
    3710, 3750, 3790, 3830, 3870, 3910, 3950, 3990, 3995, 4000
  };
  UINT32  I;

  if (Mv <= Ocv[0]) {
    return 0;
  }

  for (I = 1; I < 21; I++) {
    if (Mv < Ocv[I]) {
      return (I - 1) * 5 + 5 * (Mv - Ocv[I - 1]) / (Ocv[I] - Ocv[I - 1]);
    }
  }

  return 100;
}

//
// The charge screen as a state machine. The caller feeds it a reading now and
// then and does what Out says. Short press: screen on/off. Hold for 2 s:
// continue booting. (U-Boot refuses below its exit voltage, 3090 mV, but that
// is under the 3105 mV extreme-low limit where the keys do nothing anyway.)
// Below 3105 mV ("extreme low") the keys do nothing, the screen goes off after
// 5 s and stays off until the battery is above that. Charger gone: power off.
//
typedef enum {
  PowerActNone,
  PowerActContinueBoot,
  PowerActPowerOff
} POWER_ACTION;

typedef struct {
  BOOLEAN  Started;
  BOOLEAN  ScreenOn;
  BOOLEAN  Extreme;
  BOOLEAN  KeyDown;
  BOOLEAN  LongHandled;
  UINT64   ScreenSince;    // ms: when the screen last came on
  UINT64   KeySince;
} POWER_CHARGE_STATE;

typedef struct {
  POWER_ACTION  Action;
  BOOLEAN       Redraw;     // draw the screen (ScreenOn tells whether on or black)
} POWER_CHARGE_OUT;

STATIC
VOID
PowerChargeStep (
  IN OUT POWER_CHARGE_STATE  *S,
  IN  UINT64                 NowMs,
  IN  UINT32                 Mv,
  IN  BOOLEAN                Plug,
  IN  BOOLEAN                KeyDown,
  IN  BOOLEAN                KeyTap,
  OUT POWER_CHARGE_OUT       *Out
  )
{
  BOOLEAN  Extreme;

  Out->Action = PowerActNone;
  Out->Redraw = FALSE;

  if (!Plug) {
    Out->Action = PowerActPowerOff;
    return;
  }

  Extreme = (Mv != 0) && (Mv < POWER_EXTREME_LOW_MV);

  if (!S->Started) {
    S->Started     = TRUE;
    S->ScreenOn    = TRUE;
    S->Extreme     = Extreme;
    S->ScreenSince = NowMs;
    Out->Redraw    = TRUE;
  }

  if (Extreme != S->Extreme) {
    S->Extreme = Extreme;
    if (!Extreme) {
      // Back above the threshold: the screen comes on.
      S->ScreenOn    = TRUE;
      S->ScreenSince = NowMs;
      Out->Redraw    = TRUE;
    }
  }

  if (S->Extreme) {
    if (S->ScreenOn && (NowMs - S->ScreenSince >= POWER_EXTREME_OFF_MS)) {
      S->ScreenOn = FALSE;
      Out->Redraw = TRUE;
    }

    S->KeyDown = FALSE;
    return;
  }

  if (KeyDown && !S->KeyDown) {
    S->KeyDown     = TRUE;
    S->KeySince    = NowMs;
    S->LongHandled = FALSE;
  } else if (KeyDown && S->KeyDown && !S->LongHandled && (NowMs - S->KeySince >= POWER_LONG_PRESS_MS)) {
    S->LongHandled = TRUE;
    if (!S->ScreenOn) {
      S->ScreenOn    = TRUE;
      S->ScreenSince = NowMs;
    }

    Out->Action = PowerActContinueBoot;
    return;
  } else if (!KeyDown && S->KeyDown) {
    S->KeyDown = FALSE;
    if (!S->LongHandled) {
      KeyTap = TRUE;
    }
  }

  if (KeyTap && !KeyDown) {
    S->ScreenOn = !S->ScreenOn;
    if (S->ScreenOn) {
      S->ScreenSince = NowMs;
    }

    Out->Redraw = TRUE;
  }

  if (S->ScreenOn && (NowMs - S->ScreenSince >= POWER_SCREEN_OFF_MS)) {
    S->ScreenOn = FALSE;
    Out->Redraw = TRUE;
  }
}

#endif

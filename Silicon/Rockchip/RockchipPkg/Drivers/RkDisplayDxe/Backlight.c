/** @file
  PWM backlight on a Rockchip v3 PWM channel (rockchip,rk3328-pwm).

  Ported from U-Boot drivers/video/pwm_backlight.c (pwm_backlight_enable,
  pwm_backlight_disable) and drivers/pwm/rk_pwm.c (pwm_data_v3:
  set_invert, set_config with the lock bit, set_enable, then the "active"
  pinctrl state).

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Library/CruLib.h>
#include <Library/DebugLib.h>
#include <Library/GpioLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include "RkDisplay.h"

#define PWM_PERIOD  0x04
#define PWM_DUTY    0x08
#define PWM_CTRL    0x0C

#define RK_PWM_ENABLE           BIT0
#define PWM_CONTINUOUS          BIT1
#define PWM_DUTY_POSITIVE       BIT3
#define PWM_INACTIVE_POSITIVE   BIT4
#define PWM_LOCK                BIT6
#define PWM_POLARITY_MASK       (BIT3 | BIT4)
#define PWM_ENABLE_CONF         (RK_PWM_ENABLE | PWM_CONTINUOUS)  // output left, LP off
#define PWM_ENABLE_CONF_MASK    (BIT0 | BIT1 | BIT2 | BIT5 | BIT8)

STATIC
VOID
PwmConfig (
  IN RK_DISPLAY  *Display,
  IN UINT32      DutyNs
  )
{
  CONST RK_BACKLIGHT_DESC  *Bl;
  UINT32                   Ctrl;
  UINT32                   Polarity;

  Bl = &Display->Panel.Backlight;

  // rk_pwm_set_invert: normal is duty high, inactive low
  Polarity = Bl->Inverted ? PWM_INACTIVE_POSITIVE : PWM_DUTY_POSITIVE;

  // Lock so the new period and duty take effect together at the next period
  Ctrl = MmioRead32 (Bl->PwmBase + PWM_CTRL) | PWM_LOCK;
  MmioWrite32 (Bl->PwmBase + PWM_CTRL, Ctrl);
  MmioWrite32 (Bl->PwmBase + PWM_PERIOD, RkPwmCount (Display->PwmClockHz, Bl->PeriodNs));
  MmioWrite32 (Bl->PwmBase + PWM_DUTY, RkPwmCount (Display->PwmClockHz, DutyNs));
  Ctrl = (Ctrl & ~(PWM_POLARITY_MASK | PWM_LOCK)) | Polarity;
  MmioWrite32 (Bl->PwmBase + PWM_CTRL, Ctrl);
}

STATIC
VOID
PwmSetEnable (
  IN RK_DISPLAY  *Display,
  IN BOOLEAN     Enable
  )
{
  CONST RK_BACKLIGHT_DESC  *Bl;
  CONST RK_PINMUX          *Mux;
  UINT32                   Ctrl;
  UINT32                   Index;

  Bl   = &Display->Panel.Backlight;
  Ctrl = MmioRead32 (Bl->PwmBase + PWM_CTRL) & ~PWM_ENABLE_CONF_MASK;
  if (Enable) {
    Ctrl |= PWM_ENABLE_CONF;
  }

  MmioWrite32 (Bl->PwmBase + PWM_CTRL, Ctrl);
  if (!Enable) {
    return;
  }

  // pinctrl_select_state(dev, "active")
  for (Index = 0; Index < Bl->PinCount; Index++) {
    Mux = &Bl->Pins[Index];
    GpioPinSetFunction (Mux->Bank, Mux->Pin, Mux->Function);
    if (Mux->Pull >= 0) {
      GpioPinSetPull (Mux->Bank, Mux->Pin, (GPIO_PIN_PULL)Mux->Pull);
    }

    DEBUG ((DEBUG_INFO, "Backlight: pin GPIO%u_%u function %u pull %d\n", Mux->Bank, Mux->Pin, Mux->Function, Mux->Pull));
  }
}

EFI_STATUS
RkBacklightEnable (
  IN OUT RK_DISPLAY  *Display
  )
{
  CONST RK_BACKLIGHT_DESC  *Bl;
  UINT32                   DutyNs;

  Bl = &Display->Panel.Backlight;
  if (!Bl->Present) {
    return EFI_NOT_FOUND;
  }

  if (Bl->Supply.Present) {
    RkSupplySet (Display->Panel.Fdt, &Bl->Supply, TRUE);
    MicroSecondDelay (120 * 1000);
  }

  Display->PwmClockHz = CruEnablePwmClock (Bl->PwmBlock);
  if (Display->PwmClockHz == 0) {
    DEBUG ((DEBUG_ERROR, "Backlight: no clock for PWM block %u\n", Bl->PwmBlock));
    return EFI_DEVICE_ERROR;
  }

  DutyNs = RkBacklightDutyNs (Bl);
  PwmConfig (Display, DutyNs);
  PwmSetEnable (Display, TRUE);
  MicroSecondDelay (10 * 1000);
  RkGpioSet (&Bl->Enable, TRUE);

  DEBUG ((
    DEBUG_INFO,
    "Backlight: PWM clock %lu Hz, period %u ns = %u counts, duty %u ns = %u counts, CTRL 0x%08x PERIOD %u DUTY %u\n",
    Display->PwmClockHz,
    Bl->PeriodNs,
    RkPwmCount (Display->PwmClockHz, Bl->PeriodNs),
    DutyNs,
    RkPwmCount (Display->PwmClockHz, DutyNs),
    MmioRead32 (Bl->PwmBase + PWM_CTRL),
    MmioRead32 (Bl->PwmBase + PWM_PERIOD),
    MmioRead32 (Bl->PwmBase + PWM_DUTY)
    ));
  return EFI_SUCCESS;
}

/**
  Duty 0; the PWM keeps running unless an enable GPIO can do the
  switching (U-Boot: some boards use 0%/100% duty as the switch).
**/
VOID
RkBacklightDisable (
  IN RK_DISPLAY  *Display
  )
{
  CONST RK_BACKLIGHT_DESC  *Bl;

  Bl = &Display->Panel.Backlight;
  if (!Bl->Present) {
    return;
  }

  if (Display->PwmClockHz == 0) {
    // Taking over from U-Boot: the clock is whatever it left running.
    Display->PwmClockHz = CruEnablePwmClock (Bl->PwmBlock);
  }

  PwmConfig (Display, 0);
  if (Bl->Enable.Valid) {
    PwmSetEnable (Display, FALSE);
  }

  MicroSecondDelay (10 * 1000);
  RkGpioSet (&Bl->Enable, FALSE);
  if (Bl->Supply.Present) {
    RkSupplySet (Display->Panel.Fdt, &Bl->Supply, FALSE);
    MicroSecondDelay (120 * 1000);
  }
}

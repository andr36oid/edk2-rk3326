/** @file
  The DSI panel: GPIOs, rails, reset and the init/exit command sequences.

  Ported from U-Boot drivers/video/drm/rockchip_panel.c in its
  CONFIG_PLATFORM_ODROID_GOADV form (the one andr36oid's U-Boot is built
  with: backlight-supply handled by the panel, reset released on
  unprepare), drm_mipi_dsi.c (generic and DCS writes) and
  power/regulator/fixed.c (GPIO-switched supplies).

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/GpioLib.h>
#include <Library/RkPmicLib.h>
#include <Library/TimerLib.h>
#include "RkDisplay.h"

VOID
RkGpioSet (
  IN CONST RK_GPIO_SPEC  *Gpio,
  IN BOOLEAN             Active
  )
{
  if (Gpio->Valid) {
    GpioPinWrite (Gpio->Bank, Gpio->Pin, Gpio->ActiveLow ? !Active : Active);
  }
}

/**
  gpio_request_by_name(..., GPIOD_IS_OUT[_ACTIVE]): GPIO function, value,
  then output.
**/
STATIC
VOID
GpioClaim (
  IN CONST CHAR8         *What,
  IN CONST RK_GPIO_SPEC  *Gpio,
  IN BOOLEAN             Active
  )
{
  if (!Gpio->Valid) {
    return;
  }

  GpioPinSetFunction (Gpio->Bank, Gpio->Pin, 0);
  RkGpioSet (Gpio, Active);
  GpioPinSetDirection (Gpio->Bank, Gpio->Pin, GPIO_PIN_OUTPUT);
  DEBUG ((
    DEBUG_INFO,
    "Panel: %a GPIO%u pin %u output, %a (pad %a)\n",
    What,
    Gpio->Bank,
    Gpio->Pin,
    Active ? "active" : "inactive",
    GpioPinGetLevel (Gpio->Bank, Gpio->Pin) ? "high" : "low"
    ));
}

/**
  What U-Boot's probes do to the pins: panel enable and reset, backlight
  enable go to their inactive level; a regulator-fixed with
  enable-active-high comes on right away.
**/
VOID
RkPanelGpioInit (
  IN CONST RK_PANEL_DESC  *Panel
  )
{
  GpioClaim ("enable", &Panel->EnableGpio, FALSE);
  GpioClaim ("reset", &Panel->ResetGpio, FALSE);
  GpioClaim ("backlight enable", &Panel->Backlight.Enable, FALSE);
  if (Panel->PowerSupply.Fixed) {
    GpioClaim (Panel->PowerSupply.Name, &Panel->PowerSupply.Gpio, Panel->PowerSupply.EnableActiveHigh);
  }

  if (Panel->BacklightSupply.Fixed) {
    GpioClaim (Panel->BacklightSupply.Name, &Panel->BacklightSupply.Gpio, Panel->BacklightSupply.EnableActiveHigh);
  }
}

EFI_STATUS
RkSupplySet (
  IN CONST VOID       *Fdt,
  IN CONST RK_SUPPLY  *Supply,
  IN BOOLEAN          Enable
  )
{
  EFI_STATUS  Status;

  if (!Supply->Present) {
    return EFI_SUCCESS;
  }

  if (Supply->Fixed) {
    if (!Supply->Gpio.Valid) {
      // A fixed rail without a switch is always on.
      Status = Enable ? EFI_SUCCESS : EFI_UNSUPPORTED;
    } else {
      RkGpioSet (&Supply->Gpio, Enable);
      if (Enable && (Supply->StartupDelayUs != 0)) {
        MicroSecondDelay (Supply->StartupDelayUs);
      }

      Status = EFI_SUCCESS;
    }
  } else {
    Status = RkPmicSetRegulator (Fdt, Supply->Node, Enable);
  }

  DEBUG ((
    DEBUG_INFO,
    "Panel: supply %a %a (%a, %u uV): %r\n",
    Supply->Name,
    Enable ? "on" : "off",
    Supply->Fixed ? "GPIO" : "PMIC",
    Supply->MinMicroVolts,
    Status
    ));
  return Status;
}

STATIC
EFI_STATUS
SendCmds (
  IN CONST RK_PANEL_DESC  *Panel,
  IN CONST CHAR8          *What,
  IN CONST UINT8          *Seq,
  IN UINT32               SeqLen
  )
{
  RK_PANEL_CMD  Cmd;
  UINT32        Offset;
  UINT32        Index;
  UINT8         MsgType;
  EFI_STATUS    Status;

  Offset = 0;
  for (Index = 0; RkPanelNextCmd (Seq, SeqLen, &Offset, &Cmd); Index++) {
    MsgType = RkPanelCmdMsgType (Cmd.Type, Cmd.Length);
    DEBUG ((
      DEBUG_INFO,
      "Panel: %a %u: type 0x%02x len %u [%02x %02x %02x %02x] -> 0x%02x, delay %u ms\n",
      What,
      Index,
      Cmd.Type,
      Cmd.Length,
      Cmd.Length > 0 ? Cmd.Payload[0] : 0,
      Cmd.Length > 1 ? Cmd.Payload[1] : 0,
      Cmd.Length > 2 ? Cmd.Payload[2] : 0,
      Cmd.Length > 3 ? Cmd.Payload[3] : 0,
      MsgType,
      Cmd.DelayMs
      ));
    if (MsgType == 0) {
      DEBUG ((DEBUG_ERROR, "Panel: unsupported command data type 0x%02x\n", Cmd.Type));
      return EFI_UNSUPPORTED;
    }

    Status = RkDsiTransfer (Panel->DsiBase, Panel->ModeFlags, (UINT8)Panel->Channel, MsgType, Cmd.Payload, Cmd.Length);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Panel: failed to write %a %u: %r\n", What, Index, Status));
      return Status;
    }

    if (Cmd.DelayMs != 0) {
      MicroSecondDelay (Cmd.DelayMs * 1000);
    }
  }

  if (Offset != SeqLen) {
    DEBUG ((DEBUG_WARN, "Panel: %a: %u trailing bytes not sent\n", What, SeqLen - Offset));
  }

  DEBUG ((DEBUG_INFO, "Panel: %a: %u commands sent\n", What, Index));
  return EFI_SUCCESS;
}

/**
  panel_simple_prepare: rails, enable, reset pulse, init commands.
  U-Boot gives up when the backlight rail fails; this carries on, the
  panel can still show something.
**/
EFI_STATUS
RkPanelPrepare (
  IN CONST RK_PANEL_DESC  *Panel
  )
{
  RkSupplySet (Panel->Fdt, &Panel->PowerSupply, !Panel->PowerInvert);
  if (EFI_ERROR (RkSupplySet (Panel->Fdt, &Panel->BacklightSupply, TRUE))) {
    DEBUG ((DEBUG_WARN, "Panel: backlight supply failed (U-Boot would stop here), going on\n"));
  }

  RkGpioSet (&Panel->EnableGpio, TRUE);
  MicroSecondDelay (Panel->PrepareDelayMs * 1000);
  RkGpioSet (&Panel->ResetGpio, TRUE);
  MicroSecondDelay (Panel->ResetDelayMs * 1000);
  RkGpioSet (&Panel->ResetGpio, FALSE);
  MicroSecondDelay (Panel->InitDelayMs * 1000);
  DEBUG ((
    DEBUG_INFO,
    "Panel: enabled, reset pulsed (%u + %u + %u ms)\n",
    Panel->PrepareDelayMs,
    Panel->ResetDelayMs,
    Panel->InitDelayMs
    ));

  if (Panel->InitSeq == NULL) {
    return EFI_SUCCESS;
  }

  return SendCmds (Panel, "init", Panel->InitSeq, Panel->InitSeqLen);
}

/**
  panel_simple_unprepare (GOADV): exit commands, reset released, enable
  off, rails off.
**/
VOID
RkPanelUnprepare (
  IN CONST RK_PANEL_DESC  *Panel
  )
{
  if (Panel->ExitSeq != NULL) {
    SendCmds (Panel, "exit", Panel->ExitSeq, Panel->ExitSeqLen);
  }

  RkGpioSet (&Panel->ResetGpio, FALSE);
  MicroSecondDelay (Panel->ResetDelayMs * 1000);
  RkGpioSet (&Panel->EnableGpio, FALSE);
  RkSupplySet (Panel->Fdt, &Panel->PowerSupply, Panel->PowerInvert);
  RkSupplySet (Panel->Fdt, &Panel->BacklightSupply, FALSE);
  MicroSecondDelay (Panel->UnprepareDelayMs * 1000);
}

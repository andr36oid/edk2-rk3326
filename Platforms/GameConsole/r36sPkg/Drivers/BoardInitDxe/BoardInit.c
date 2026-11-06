/** @file
  Board init in C: what U-Boot does before it hands over to Linux.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/FdtLib.h>
#include <Library/CruLib.h>
#include <Library/GpioLib.h>
#include <Library/RkClockInitLib.h>

#include "BoardInit.h"

STATIC
VOID
GrfWrite (
  IN UINTN                  Base,
  IN CONST GRF_REG_WRITE    *W,
  IN UINTN                  Count
  )
{
  UINTN  I;

  for (I = 0; I < Count; I++) {
    MmioWrite32 (Base + W[I].Offset, W[I].Value);
    DEBUG ((DEBUG_VERBOSE, "BoardInit: GRF 0x%lX <- 0x%X, reads 0x%X\n", Base + W[I].Offset, W[I].Value, MmioRead32 (Base + W[I].Offset)));
  }
}

//
// True if some node has enable-gpios on GPIO0_B5: then that pin is the screen's
// enable, not a charge LED (the D-28S), and driving it low blanks the screen.
//
STATIC
BOOLEAN
ChargeLedIsScreenEnable (
  IN CONST VOID  *Fdt
  )
{
  INT32         Node;
  INT32         Depth;
  INT32         Len;
  INT32         Bank;
  CONST UINT32  *Gpios;
  CONST CHAR8   *Name;

  Depth = 0;
  for (Node = FdtNextNode (Fdt, -1, &Depth); Node >= 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
    Gpios = FdtGetProp (Fdt, Node, "enable-gpios", &Len);
    if ((Gpios == NULL) || (Len < 8) || (Fdt32ToCpu (Gpios[1]) != BOARD_CHG_LED_PIN)) {
      continue;
    }

    Bank = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Gpios[0]));
    Name = (Bank >= 0) ? FdtGetName (Fdt, Bank, NULL) : NULL;
    if ((Name != NULL) && (AsciiStrnCmp (Name, "gpio0@", 6) == 0)) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
VOID
GpioOut (
  IN CONST BOARD_GPIO_OUT  *O,
  IN CONST CHAR8           *Name
  )
{
  // Level first, so the pad never glitches to the other value.
  GpioPinWrite (O->Bank, O->Pin, O->Level);
  GpioPinSetDirection (O->Bank, O->Pin, GPIO_PIN_OUTPUT);
  DEBUG ((DEBUG_INFO, "BoardInit: %a GPIO%d_%d -> %d\n", Name, O->Bank, O->Pin, O->Level));
}

EFI_STATUS
BoardInitRun (
  IN CONST BOARD_INIT_PLAN  *Plan
  )
{
  STATIC CONST GRF_REG_WRITE  Switch[] = { SWITCH_GPIO_WRITES };
  STATIC CONST GRF_REG_WRITE  Emmc[]   = { EMMC_PIN_WRITES };
  STATIC CONST BOARD_GPIO_OUT Rumble   = BOARD_GPIO_RUMBLE;
  STATIC CONST BOARD_GPIO_OUT PwrLed   = BOARD_GPIO_PWR_LED;
  STATIC CONST BOARD_GPIO_OUT ChgLed   = BOARD_GPIO_CHG_LED;
  STATIC CONST BOARD_GPIO_OUT WifiEn   = BOARD_GPIO_WIFI_EN;
  EFI_STATUS                  Status;

  DEBUG ((DEBUG_INFO, "BoardInit: force %d pins %d emmc %d fdt %p\n", Plan->Force, Plan->Pins, Plan->EmmcPins, Plan->Fdt));

  Status = RkClockInit (Plan->Force);
  DEBUG ((DEBUG_INFO, "BoardInit: clocks %r\n", Status));

  // IO domains, as io_domain_init does after the regulators are up.
  MmioWrite32 (SYS_GRF + GRF_IO_VSEL, GRF_WRITE (IO_VSEL_SYS_MASK, IO_VSEL_SYS_VALUE));
  MmioWrite32 (PMU_GRF + PMUGRF_IO_VSEL, GRF_WRITE (IO_VSEL_PMU_MASK, IO_VSEL_PMU_VALUE));
  DEBUG ((
    DEBUG_INFO,
    "BoardInit: IO_VSEL sys 0x%X pmu 0x%X\n",
    MmioRead32 (SYS_GRF + GRF_IO_VSEL),
    MmioRead32 (PMU_GRF + PMUGRF_IO_VSEL)
    ));

  if (Plan->Pins) {
    MmioWrite32 (SYS_GRF + GRF_IOFUNC_CON0, GRF_WRITE (IOFUNC_UART2_MASK, IOFUNC_UART2_M1));
    GrfWrite (SYS_GRF, Switch, ARRAY_SIZE (Switch));
  }

  // After the button pins, as in U-Boot: the eMMC takes GPIO1_A0-A7 back.
  if (Plan->EmmcPins) {
    GrfWrite (SYS_GRF, Emmc, ARRAY_SIZE (Emmc));
    DEBUG ((DEBUG_INFO, "BoardInit: eMMC pins set (1A iomux 0x%X, 1A pull 0x%X)\n", MmioRead32 (SYS_GRF), MmioRead32 (SYS_GRF + GRF_GPIO1A_P)));
  }

  if (Plan->Pins) {
    CruEnableGpioClocks ();
    GpioOut (&Rumble, "rumble");
    GpioOut (&PwrLed, "power LED");
    GpioOut (&WifiEn, "Wi-Fi enable");
    //
    // Without U-Boot's FDT there is no way to tell, and a wrong guess blanks a
    // D-28S: leave the pin alone then.
    //
    if (Plan->Fdt != NULL) {
      if (ChargeLedIsScreenEnable (Plan->Fdt)) {
        DEBUG ((DEBUG_INFO, "BoardInit: GPIO0_B5 is the screen enable, charge LED left alone\n"));
      } else {
        GpioOut (&ChgLed, "charge LED");
      }
    } else {
      DEBUG ((DEBUG_INFO, "BoardInit: no FDT, charge LED pin GPIO0_B5 left alone\n"));
    }
  }

  return Status;
}

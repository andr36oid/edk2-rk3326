/** @file
  PX30 USB OTG host port setup, from what the Linux clock, power-domain and
  Inno USB2 PHY drivers do for this port (the DTS has it as a host: the ID
  pin does not follow the cable).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CruLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/GpioLib.h>
#include <Library/IoLib.h>
#include <Library/RkPmicLib.h>
#include <Library/RkUsbPlatformLib.h>
#include <Library/TimerLib.h>
#include "RkUsbPlatformRegs.h"
#include "UsbVbusPolicy.h"

#define GSNPSID_REG      0x40
#define POLL_STEP_US     10
#define POLL_MAX_STEPS   1000       // 10 ms for the PMU to answer

// Time the charger gets to show itself once the port is back in charge mode.
#define CHARGE_PROBE_SETTLE_US  150000

STATIC
EFI_STATUS
WaitBit (
  IN UINTN    Reg,
  IN UINT32   Mask,
  IN BOOLEAN  Set
  )
{
  UINTN  Step;

  for (Step = 0; Step < POLL_MAX_STEPS; Step++) {
    if (((MmioRead32 (Reg) & Mask) != 0) == Set) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (POLL_STEP_US);
  }

  return EFI_TIMEOUT;
}

/**
  PD_USB on, idle request released. The boot ROM and U-Boot leave it on; this
  only does something if a previous stage turned it off.
**/
STATIC
EFI_STATUS
PowerDomainOn (
  VOID
  )
{
  EFI_STATUS  Status;

  DEBUG ((DEBUG_INFO, "USB: PMU pwrdn_st 0x%x idle_st 0x%x before\n", MmioRead32 (PMU_PWRDN_ST), MmioRead32 (PMU_BUS_IDLE_ST)));

  if ((MmioRead32 (PMU_PWRDN_ST) & (1U << PD_USB_PWR_BIT)) != 0) {
    MmioWrite32 (PMU_PWRDN_CON, HIWORD (1U << PD_USB_PWR_BIT, 0));
    Status = WaitBit (PMU_PWRDN_ST, 1U << PD_USB_PWR_BIT, FALSE);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "USB: PD_USB did not power up (pwrdn_st 0x%x)\n", MmioRead32 (PMU_PWRDN_ST)));
      return Status;
    }
  }

  if ((MmioRead32 (PMU_BUS_IDLE_ST) & (1U << PD_USB_IDLE_BIT)) != 0) {
    MmioWrite32 (PMU_BUS_IDLE_REQ, HIWORD (1U << PD_USB_IDLE_BIT, 0));
    Status = WaitBit (PMU_BUS_IDLE_ST, 1U << PD_USB_IDLE_BIT, FALSE);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "USB: PD_USB idle not released (idle_st 0x%x)\n", MmioRead32 (PMU_BUS_IDLE_ST)));
      return Status;
    }
  }

  return EFI_SUCCESS;
}

STATIC
VOID
UngateClocks (
  VOID
  )
{
  // Gate registers: a set bit stops the clock, so write the bit as 0.
  MmioWrite32 (
    CRU_CLKGATE_CON (7),
    HIWORD (CRU_GATE7_HCLK_USB | CRU_GATE7_OTG_ADP | CRU_GATE7_USB_NIU | CRU_GATE7_HCLK_OTG, 0)
    );
  MmioWrite32 (CRU_CLKGATE_CON (16), HIWORD (CRU_GATE16_PCLK_USBGRF, 0));

  // The PHY's 24 MHz input: crystal, gate open.
  MmioWrite32 (PMUCRU_CLKSEL_CON (2), HIWORD (1U << PMUCRU_USBPHY_SEL_BIT, 0));
  MmioWrite32 (PMUCRU_CLKGATE_CON (1), HIWORD (1U << PMUCRU_USBPHY_GATE_BIT, 0));

  DEBUG ((
    DEBUG_INFO,
    "USB: clkgate7 0x%x (want bits 2-5 clear) gate16 0x%x pmu gate1 0x%x\n",
    MmioRead32 (CRU_CLKGATE_CON (7)),
    MmioRead32 (CRU_CLKGATE_CON (16)),
    MmioRead32 (PMUCRU_CLKGATE_CON (1))
    ));
}

/**
  The OTG port as a host with the PHY awake, as rockchip_usb2phy_apply_otg_mode
  (USB_DR_MODE_HOST) and rockchip_usb2phy_power_on leave it.
**/
STATIC
VOID
PhyHostMode (
  VOID
  )
{
  // rk3328_usb2phy_tuning
  MmioWrite32 (USB2PHY_GRF + PHY_TUNE0_OFF, PHY_TUNE0_VAL);
  MmioWrite32 (USB2PHY_GRF + PHY_TUNE1_OFF, PHY_TUNE1_VAL);
  MmioWrite32 (USB2PHY_GRF + PHY_TUNE2_OFF, PHY_TUNE2_VAL);
  MmioWrite32 (USB2PHY_GRF + PHY_TUNE3_OFF, PHY_TUNE3_VAL);

  // 480 MHz UTMI clock out of the PHY on (clk480m prepare)
  MmioWrite32 (USB2PHY_GRF + PHY_CLKOUT_OFF, PHY_CLKOUT_ENABLE);

  // The ID pin is unreliable: the GRF drives it low (A device = host), and the
  // VBUS comparator, which only matters to a device, is powered down.
  MmioWrite32 (USB2PHY_GRF + PHY_OTG_CTL_OFF, PHY_IDDIG_OUTPUT_LOW);
  MmioWrite32 (USB2PHY_GRF + PHY_OTG_CTL_OFF, PHY_IDDIG_EN);
  MmioWrite32 (USB2PHY_GRF + PHY_VBUS_DET_OFF, PHY_VBUS_DET_DISABLE);

  // Out of suspend, then wait for the UTMI clock to settle (the driver sleeps
  // 1.5 to 2 ms).
  MmioWrite32 (USB2PHY_GRF + PHY_OTG_CTL_OFF, PHY_SUSPEND_RELEASE);
  MicroSecondDelay (2000);

  DEBUG ((
    DEBUG_INFO,
    "USB: phy ctl 0x%x clkout 0x%x vbus_det 0x%x tune 0x%x/0x%x/0x%x/0x%x\n",
    MmioRead32 (USB2PHY_GRF + PHY_OTG_CTL_OFF),
    MmioRead32 (USB2PHY_GRF + PHY_CLKOUT_OFF),
    MmioRead32 (USB2PHY_GRF + PHY_VBUS_DET_OFF),
    MmioRead32 (USB2PHY_GRF + PHY_TUNE0_OFF),
    MmioRead32 (USB2PHY_GRF + PHY_TUNE1_OFF),
    MmioRead32 (USB2PHY_GRF + PHY_TUNE2_OFF),
    MmioRead32 (USB2PHY_GRF + PHY_TUNE3_OFF)
    ));
}

EFI_STATUS
EFIAPI
RkUsbPlatformPowerOn (
  OUT UINT64  *Base
  )
{
  EFI_STATUS  Status;
  UINT32      Id;

  if (Base == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = PowerDomainOn ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UngateClocks ();
  PhyHostMode ();

  // The core sees a running UTMI clock now: pulse its resets so it starts clean.
  CruAssertSoftReset (CRU_SOFTRST_USB_CON, SRST_USB2OTG_H_BIT);
  CruAssertSoftReset (CRU_SOFTRST_USB_CON, SRST_USB2OTG_BIT);
  CruAssertSoftReset (CRU_SOFTRST_USB_CON, SRST_USB2OTG_ADP_BIT);
  MicroSecondDelay (50);
  CruDeassertSoftReset (CRU_SOFTRST_USB_CON, SRST_USB2OTG_ADP_BIT);
  CruDeassertSoftReset (CRU_SOFTRST_USB_CON, SRST_USB2OTG_BIT);
  CruDeassertSoftReset (CRU_SOFTRST_USB_CON, SRST_USB2OTG_H_BIT);
  MicroSecondDelay (100);

  // Reading the core of a domain that is off would hang the bus, so only now.
  Id = MmioRead32 (RK_USB_OTG_BASE + GSNPSID_REG);
  DEBUG ((DEBUG_INFO, "USB: DWC2 GSNPSID 0x%08x\n", Id));
  if (((Id & 0xFFFFF000U) != 0x4F542000U) && ((Id & 0xFFFFF000U) != 0x4F543000U)) {
    DEBUG ((DEBUG_ERROR, "USB: no DWC2 core answers at 0x%lx\n", RK_USB_OTG_BASE));
    return EFI_DEVICE_ERROR;
  }

  *Base = RK_USB_OTG_BASE;
  return EFI_SUCCESS;
}

//
// Board device tree: which 5 V switches this board has.
//

typedef struct {
  BOOLEAN    HaveHostGpio;
  UINT8      HostBank;
  UINT8      HostPin;
  BOOLEAN    HostActiveHigh;
  BOOLEAN    HaveDcDet;
  UINT8      DcBank;
  UINT8      DcPin;
  BOOLEAN    DcActiveHigh;
  INT32      BoostNode;           // PMIC boost regulator node, -1 if none
  BOOLEAN    BoostOnlyForOtg;
  BOOLEAN    HostSupplyProp;
} USB_BOARD;

/**
  Read a "<&gpioN pin flags>" property into bank, pin and polarity.
**/
STATIC
BOOLEAN
ReadGpioProp (
  IN  CONST VOID  *Fdt,
  IN  INT32       Node,
  IN  CONST CHAR8 *PropName,
  OUT UINT8       *Bank,
  OUT UINT8       *Pin,
  OUT BOOLEAN     *ActiveLow
  )
{
  CONST UINT32  *Prop;
  CONST UINT32  *Reg;
  INT32         Len;
  INT32         RegLen;
  INT32         BankNode;
  UINT64        Addr;
  UINT32        Pin32;

  Prop = FdtGetProp (Fdt, Node, PropName, &Len);
  if ((Prop == NULL) || (Len < 12)) {
    return FALSE;
  }

  BankNode = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Prop[0]));
  if (BankNode < 0) {
    return FALSE;
  }

  Reg = FdtGetProp (Fdt, BankNode, "reg", &RegLen);
  if ((Reg == NULL) || (RegLen < 8)) {
    return FALSE;
  }

  // Two address cells on this SoC, one if a DTB was built that way.
  Addr = (RegLen >= 16) ? (((UINT64)Fdt32ToCpu (Reg[0]) << 32) | Fdt32ToCpu (Reg[1])) : Fdt32ToCpu (Reg[0]);

  if (Addr == GPIO_BANK0) {
    *Bank = 0;
  } else if ((Addr >= GPIO_BANK1) && (Addr < GPIO_BANK1 + 4 * 0x10000) && (((Addr - GPIO_BANK1) & 0xFFFF) == 0)) {
    *Bank = (UINT8)(1 + ((Addr - GPIO_BANK1) >> 16));
  } else {
    return FALSE;
  }

  Pin32 = Fdt32ToCpu (Prop[1]);
  if (Pin32 > 31) {
    return FALSE;
  }

  *Pin       = (UINT8)Pin32;
  *ActiveLow = (Fdt32ToCpu (Prop[2]) & 1) != 0;
  return TRUE;
}

STATIC
VOID
ScanBoard (
  IN  CONST VOID  *Fdt,
  OUT USB_BOARD   *Board
  )
{
  INT32         Node;
  INT32         Depth;
  INT32         Len;
  CONST CHAR8   *Name;
  BOOLEAN       Low;
  BOOLEAN       EnableHigh;

  ZeroMem (Board, sizeof (*Board));
  Board->BoostNode = -1;

  Depth = 0;
  for (Node = FdtNextNode (Fdt, 0, &Depth); Node >= 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
    Name = FdtGetProp (Fdt, Node, "regulator-name", &Len);
    if ((Name != NULL) && (Len > 0)) {
      if (AsciiStrCmp (Name, "vcc_host") == 0) {
        if (ReadGpioProp (Fdt, Node, "gpio", &Board->HostBank, &Board->HostPin, &Low)) {
          // regulator-fixed: only enable-active-high decides, the flags cell
          // is ignored (the kernel's fixed.c and gpiolib-of quirk); without it
          // the switch is active low
          EnableHigh = FdtGetProp (Fdt, Node, "enable-active-high", &Len) != NULL;
          Board->HaveHostGpio   = TRUE;
          Board->HostActiveHigh = EnableHigh;
        }
      } else if (AsciiStrCmp (Name, "boost") == 0) {
        Board->BoostNode = Node;
      }
    }

    if (FdtGetProp (Fdt, Node, "andr36oid,boost-only-for-otg", &Len) != NULL) {
      Board->BoostOnlyForOtg = TRUE;
    }

    if (FdtGetProp (Fdt, Node, "andr36oid,usb-host-supply", &Len) != NULL) {
      Board->HostSupplyProp = TRUE;
    }

    if (!Board->HaveDcDet && ReadGpioProp (Fdt, Node, "dc_det_gpio", &Board->DcBank, &Board->DcPin, &Low)) {
      Board->HaveDcDet    = TRUE;
      Board->DcActiveHigh = !Low;
    }
  }
}

STATIC
VOID
SetHostGpio (
  IN CONST USB_BOARD  *Board,
  IN BOOLEAN          On
  )
{
  if (!Board->HaveHostGpio) {
    return;
  }

  CruEnableGpioClocks ();
  GpioPinSetFunction (Board->HostBank, Board->HostPin, 0);
  GpioPinWrite (Board->HostBank, Board->HostPin, (BOOLEAN)(On == Board->HostActiveHigh));
  GpioPinSetDirection (Board->HostBank, Board->HostPin, GPIO_PIN_OUTPUT);
  DEBUG ((DEBUG_INFO, "USB: vcc_host GPIO%u_%u %a (reads %d)\n", Board->HostBank, Board->HostPin, On ? "on" : "off", GpioPinGetLevel (Board->HostBank, Board->HostPin)));
}

STATIC
BOOLEAN
ChargerIn (
  IN CONST USB_BOARD  *Board
  )
{
  RK_POWER_STATUS  Power;
  BOOLEAN          Pin = FALSE;
  BOOLEAN          Pmic = FALSE;

  if (Board->HaveDcDet) {
    CruEnableGpioClocks ();
    GpioPinSetFunction (Board->DcBank, Board->DcPin, 0);
    GpioPinSetDirection (Board->DcBank, Board->DcPin, GPIO_PIN_INPUT);
    Pin = (GpioPinGetLevel (Board->DcBank, Board->DcPin) == Board->DcActiveHigh);
  }

  if (!EFI_ERROR (RkPmicGetPowerStatus (&Power))) {
    Pmic = Power.ChargerPresent;
  }

  DEBUG ((DEBUG_INFO, "USB: charger: dc_det pin %d, pmic %d\n", Pin, Pmic));
  return Pin || Pmic;
}

EFI_STATUS
EFIAPI
RkUsbPlatformApplyFdt (
  IN CONST VOID  *Fdt OPTIONAL
  )
{
  USB_BOARD         Board;
  USB_VBUS_INPUT    In;
  USB_VBUS_DECISION Decision;

  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0)) {
    DEBUG ((DEBUG_INFO, "USB: no board device tree, 5 V supply left as the boot stage had it\n"));
    return EFI_SUCCESS;
  }

  ScanBoard (Fdt, &Board);
  In.SharedWithCharger = Board.BoostOnlyForOtg || Board.HostSupplyProp;
  DEBUG ((
    DEBUG_INFO,
    "USB: board: vcc_host gpio %d (GPIO%u_%u), boost node %d, boost-only-for-otg %d, usb-host-supply %d\n",
    Board.HaveHostGpio,
    Board.HostBank,
    Board.HostPin,
    Board.BoostNode,
    Board.BoostOnlyForOtg,
    Board.HostSupplyProp
    ));

  if (In.SharedWithCharger) {
    // Host mode hides the charger from the console: go back to charge mode
    // first, give the charger time to show, then decide.
    SetHostGpio (&Board, FALSE);
    if (Board.BoostOnlyForOtg && (Board.BoostNode >= 0)) {
      RkPmicSetRegulator (Fdt, Board.BoostNode, FALSE);
    }

    MicroSecondDelay (CHARGE_PROBE_SETTLE_US);
  }

  In.ChargerPresent = ChargerIn (&Board);
  Decision = UsbVbusDecide (&In);
  if (Decision == UsbVbusCharge) {
    DEBUG ((DEBUG_INFO, "USB: charger in on a shared port, leaving it in charge mode (no host power)\n"));
    return EFI_NOT_READY;
  }

  if (Board.BoostOnlyForOtg && (Board.BoostNode >= 0)) {
    DEBUG ((DEBUG_INFO, "USB: boost on: %r\n", RkPmicSetRegulator (Fdt, Board.BoostNode, TRUE)));
  }

  SetHostGpio (&Board, TRUE);
  return EFI_SUCCESS;
}

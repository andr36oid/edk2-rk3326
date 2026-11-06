/** @file
  RK817 PMIC on I2C0 (the power chip of every supported console).

  Ported from U-Boot drivers/power/pmic/rk8xx.c and
  drivers/power/regulator/rk8xx.c, with the battery voltage maths of the
  kernel's rk817_battery.c.

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/TimerLib.h>
#include <Library/RkI2cLib.h>
#include <Library/RkPmicLib.h>
#include "Rk817Regs.h"

STATIC BOOLEAN  mReady;
STATIC BOOLEAN  mPowerKeyPressed;

STATIC
EFI_STATUS
PmicRead (
  IN  UINT8  Reg,
  OUT UINT8  *Value
  )
{
  return RkI2cRead (RK817_I2C_BUS, RK817_I2C_ADDR, Reg, Value, 1);
}

STATIC
EFI_STATUS
PmicWrite (
  IN UINT8  Reg,
  IN UINT8  Value
  )
{
  return RkI2cWrite (RK817_I2C_BUS, RK817_I2C_ADDR, Reg, &Value, 1);
}

STATIC
EFI_STATUS
PmicUpdate (
  IN UINT8  Reg,
  IN UINT8  Mask,
  IN UINT8  Value
  )
{
  EFI_STATUS  Status;
  UINT8       Old;

  Status = PmicRead (Reg, &Old);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return PmicWrite (Reg, (UINT8)((Old & ~Mask) | (Value & Mask)));
}

EFI_STATUS
EFIAPI
RkPmicInit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Msb;
  UINT8       Lsb;
  UINT8       OnSource;
  UINT8       OffSource;

  if (mReady) {
    return EFI_SUCCESS;
  }

  Status = RkI2cInit (RK817_I2C_BUS, RK817_I2C_HZ);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PMIC: I2C init failed: %r\n", Status));
    return Status;
  }

  Status = PmicRead (RK817_ID_MSB, &Msb);
  if (!EFI_ERROR (Status)) {
    Status = PmicRead (RK817_ID_LSB, &Lsb);
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PMIC: no answer at I2C%u 0x%02X: %r\n", RK817_I2C_BUS, RK817_I2C_ADDR, Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "PMIC: chip id 0x%02X%02X\n", Msb, Lsb));
  if (!Rk817IsChipId (Msb, Lsb)) {
    DEBUG ((DEBUG_ERROR, "PMIC: not an RK817\n"));
    return EFI_UNSUPPORTED;
  }

  //
  // What U-Boot sets at probe (rk817_init_reg): under-voltage protection,
  // PMIC sleep pin as the sleep function, interrupt pin active low.
  //
  PmicUpdate (RK817_BUCK4_CMIN, 0x60, 0x60);
  PmicUpdate (RK817_SYS_CFG3, 0x18, 0x08);
  PmicUpdate (RK817_GPIO_INT_CFG, 0x02, 0x00);

  //
  // The battery voltage ADC has to run for the fuel gauge registers to be
  // live. U-Boot's gauge driver does this at probe.
  //
  PmicUpdate (RK817_ADC_CONFIG0, RK817_ADC_BAT_EN, RK817_ADC_BAT_EN);

  if (!EFI_ERROR (PmicRead (RK817_ON_SOURCE, &OnSource)) &&
      !EFI_ERROR (PmicRead (RK817_OFF_SOURCE, &OffSource)))
  {
    DEBUG ((DEBUG_INFO, "PMIC: RK817 on=0x%02X off=0x%02X\n", OnSource, OffSource));
  }

  mReady = TRUE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SetRegulatorByName (
  IN CONST CHAR8  *Name,
  IN UINT32       MicroVolts,
  IN BOOLEAN      HaveVolts,
  IN BOOLEAN      Enable
  )
{
  EFI_STATUS       Status;
  RK817_REGULATOR  R;
  UINT8            Sel;

  if (!Rk817ParseRegulator (Name, &R)) {
    DEBUG ((DEBUG_WARN, "PMIC: '%a' is not an RK817 regulator\n", Name));
    return EFI_NOT_FOUND;
  }

  if (Enable && HaveVolts && (R.VselReg != 0)) {
    if (!Rk817VoltageToSel (&R, MicroVolts, &Sel)) {
      DEBUG ((DEBUG_ERROR, "PMIC: %a: %u uV out of range\n", Name, MicroVolts));
      return EFI_INVALID_PARAMETER;
    }

    Status = PmicUpdate (R.VselReg, R.VselMask, Sel);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = PmicWrite (R.EnReg, Rk817EnableWrite (R.EnBit, Enable));
  DEBUG ((
    DEBUG_INFO,
    "PMIC: %a %a%a at %u uV: %r\n",
    Name,
    Enable ? "on" : "off",
    HaveVolts ? "" : " (voltage unchanged)",
    MicroVolts,
    Status
    ));
  return Status;
}

EFI_STATUS
EFIAPI
RkPmicSetRegulator (
  IN CONST VOID  *Fdt,
  IN INT32       RegulatorNode,
  IN BOOLEAN     Enable
  )
{
  EFI_STATUS    Status;
  CONST CHAR8   *Name;
  CONST UINT32  *Prop;
  INT32         Len;
  UINT32        MicroVolts;
  BOOLEAN       HaveVolts;

  if ((Fdt == NULL) || (RegulatorNode < 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = RkPmicInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Name = FdtGetName (Fdt, RegulatorNode, NULL);
  Prop = FdtGetProp (Fdt, RegulatorNode, "regulator-min-microvolt", &Len);
  HaveVolts  = (Prop != NULL) && (Len >= 4);
  MicroVolts = HaveVolts ? Fdt32ToCpu (*Prop) : 0;

  return SetRegulatorByName (Name, MicroVolts, HaveVolts, Enable);
}

EFI_STATUS
EFIAPI
RkPmicEnableBootOn (
  IN CONST VOID  *Fdt
  )
{
  EFI_STATUS    Status;
  INT32         Pmic;
  INT32         Regulators;
  INT32         Node;
  CONST UINT32  *Min;
  CONST UINT32  *Max;
  INT32         MinLen;
  INT32         MaxLen;
  BOOLEAN       Fixed;

  if (Fdt == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = RkPmicInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Pmic = FdtNodeOffsetByCompatible (Fdt, -1, "rockchip,rk817");
  if (Pmic < 0) {
    return EFI_NOT_FOUND;
  }

  Regulators = FdtSubnodeOffset (Fdt, Pmic, "regulators");
  if (Regulators < 0) {
    return EFI_NOT_FOUND;
  }

  for (Node = FdtFirstSubnode (Fdt, Regulators); Node >= 0; Node = FdtNextSubnode (Fdt, Node)) {
    if ((FdtGetProp (Fdt, Node, "regulator-boot-on", NULL) == NULL) &&
        (FdtGetProp (Fdt, Node, "regulator-always-on", NULL) == NULL))
    {
      continue;
    }

    //
    // Only fixed rails get their voltage written. The DCDCs that feed the
    // CPU and logic have a range and their level is whatever the DVFS code
    // (or the boot ROM default) set, which must not be pulled to the minimum.
    //
    Min   = FdtGetProp (Fdt, Node, "regulator-min-microvolt", &MinLen);
    Max   = FdtGetProp (Fdt, Node, "regulator-max-microvolt", &MaxLen);
    Fixed = (Min != NULL) && (Max != NULL) && (MinLen >= 4) && (MaxLen >= 4) && (*Min == *Max);
    SetRegulatorByName (
      FdtGetName (Fdt, Node, NULL),
      Fixed ? Fdt32ToCpu (*Min) : 0,
      Fixed,
      TRUE
      );
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
RkPmicGetPowerStatus (
  OUT RK_POWER_STATUS  *Status
  )
{
  EFI_STATUS  Result;
  UINT8       SysSts;
  UINT8       ChrgSts;
  UINT8       Ints;
  UINT8       Hi;
  UINT8       Lo;
  UINT8       C0H;
  UINT8       C0L;
  UINT8       C1H;
  UINT8       C1L;
  INT32       MilliVolts;

  if (Status == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status->ChargerPresent    = FALSE;
  Status->Charging          = FALSE;
  Status->PowerKeyPressed   = FALSE;
  Status->BatteryMicroVolts = 0;

  Result = RkPmicInit ();
  if (EFI_ERROR (Result)) {
    return Result;
  }

  Result = PmicRead (RK817_SYS_STS, &SysSts);
  if (!EFI_ERROR (Result)) {
    Result = PmicRead (RK817_CHRG_STS, &ChrgSts);
  }

  if (EFI_ERROR (Result)) {
    return Result;
  }

  //
  // PLUG_IN_STS is the PMIC's own view of power on its USB/DC input.
  //
  Status->ChargerPresent = (SysSts & RK817_SYS_STS_PLUG) != 0;
  Status->Charging       = Rk817IsCharging (ChrgSts);

  //
  // Power key: latched events. Read and clear them (write 1).
  //
  if (!EFI_ERROR (PmicRead (RK817_INT_STS0, &Ints))) {
    Ints &= (RK817_INT_PWRON_FALL | RK817_INT_PWRON_RISE);
    if (Ints != 0) {
      PmicWrite (RK817_INT_STS0, Ints);
    }

    mPowerKeyPressed = Rk817PowerKeyState (mPowerKeyPressed, Ints);
  }

  Status->PowerKeyPressed = mPowerKeyPressed;

  if (!EFI_ERROR (PmicRead (RK817_BAT_VOL_H, &Hi)) &&
      !EFI_ERROR (PmicRead (RK817_BAT_VOL_L, &Lo)) &&
      !EFI_ERROR (PmicRead (RK817_VCALIB0_H, &C0H)) &&
      !EFI_ERROR (PmicRead (RK817_VCALIB0_L, &C0L)) &&
      !EFI_ERROR (PmicRead (RK817_VCALIB1_H, &C1H)) &&
      !EFI_ERROR (PmicRead (RK817_VCALIB1_L, &C1L)))
  {
    MilliVolts = Rk817BatteryMilliVolts (
                   ((UINT32)Hi << 8) | Lo,
                   ((UINT32)C0H << 8) | C0L,
                   ((UINT32)C1H << 8) | C1L
                   );
    if (MilliVolts > 0) {
      Status->BatteryMicroVolts = (UINT32)MilliVolts * 1000;
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
RkPmicPowerOff (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Left;

  Status = RkPmicInit ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PmicUpdate (RK817_SYS_CFG3, RK817_DEV_OFF, RK817_DEV_OFF);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The PMIC drops the rails within milliseconds. Only get here if it didn't.
  //
  for (Left = 1000; Left > 0; Left--) {
    MicroSecondDelay (1000);
  }

  return EFI_DEVICE_ERROR;
}

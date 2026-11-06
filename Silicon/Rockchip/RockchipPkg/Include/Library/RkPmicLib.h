/** @file
  RK817 PMIC (I2C0, address 0x20 on these boards): rails, power state, off.
  Regulators are named by their FDT node in the screen/control DTB, so a
  display driver can switch on what its panel node points at
  (power-supply = <&vcc_lcd>) without knowing PMIC registers.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _RK_PMIC_LIB_H_
#define _RK_PMIC_LIB_H_

#include <Uefi.h>

typedef struct {
  BOOLEAN  ChargerPresent;    // DC/USB power in
  BOOLEAN  Charging;
  BOOLEAN  PowerKeyPressed;   // latched press/release events, see RkPmicGetPowerStatus
  UINT32   BatteryMicroVolts; // 0 if unknown
  BOOLEAN  BatteryPresent;    // CHRG_STS says a battery is connected
  BOOLEAN  PowerKeyTap;       // a press and release both seen since the last call
} RK_POWER_STATUS;

/**
  Bring up the I2C bus and check the PMIC answers (chip id).
**/
EFI_STATUS
EFIAPI
RkPmicInit (
  VOID
  );

/**
  Switch the regulator described by FDT node RegulatorNode on (at its
  regulator-min-microvolt, if set) or off.
**/
EFI_STATUS
EFIAPI
RkPmicSetRegulator (
  IN CONST VOID  *Fdt,
  IN INT32       RegulatorNode,
  IN BOOLEAN     Enable
  );

/**
  Switch on every rail the FDT marks regulator-boot-on or regulator-always-on
  (what U-Boot's regulators_enable_boot_on does). Fixed-voltage rails also get
  their voltage written; ranged rails (CPU, logic) are only enabled.
  Added in M2, the other functions are unchanged.
**/
EFI_STATUS
EFIAPI
RkPmicEnableBootOn (
  IN CONST VOID  *Fdt
  );

EFI_STATUS
EFIAPI
RkPmicGetPowerStatus (
  OUT RK_POWER_STATUS  *Status
  );

/**
  Set the charger input and charge current limits the way U-Boot's fuel gauge
  driver does (4.5 V input limit; 1500 mA in and out when a charger is
  plugged, 450 mA otherwise).
**/
EFI_STATUS
EFIAPI
RkPmicSetChargerLimits (
  VOID
  );

/**
  Cut power (the RK817 DEV_OFF bit). Does not return on success.
**/
EFI_STATUS
EFIAPI
RkPmicPowerOff (
  VOID
  );

#endif

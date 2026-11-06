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
  BOOLEAN  PowerKeyPressed;
  UINT32   BatteryMicroVolts; // 0 if unknown
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

EFI_STATUS
EFIAPI
RkPmicGetPowerStatus (
  OUT RK_POWER_STATUS  *Status
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

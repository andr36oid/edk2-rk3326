/** @file
  RK817 register facts and the arithmetic on them. No hardware access, so a
  host test can run it. The includer provides the fixed-width types, BOOLEAN,
  CHAR8, INT32, TRUE/FALSE and NULL.

  Register numbers and voltage tables: kernel drivers/regulator/rk808-regulator.c,
  include/linux/mfd/rk808.h, drivers/power/supply/rk817_battery.c and U-Boot
  drivers/power/regulator/rk8xx.c, drivers/power/pmic/rk8xx.c.

  SPDX-License-Identifier: GPL-2.0+
**/

#ifndef _RK817_REGS_H_
#define _RK817_REGS_H_

#define RK817_I2C_BUS       0
#define RK817_I2C_ADDR      0x20
#define RK817_I2C_HZ        400000

#define RK817_ID_MSB        0xED
#define RK817_ID_LSB        0xEE
#define RK817_ID_MASK       0xFFF0
#define RK817_ID_VALUE      0x8170

#define RK817_ADC_CONFIG0   0x50
#define RK817_ADC_BAT_EN    0x08
#define RK817_BAT_VOL_H     0x78
#define RK817_BAT_VOL_L     0x79
#define RK817_VCALIB0_H     0x93
#define RK817_VCALIB0_L     0x94
#define RK817_VCALIB1_H     0x95
#define RK817_VCALIB1_L     0x96

#define RK817_POWER_EN(n)   (0xB1 + (n))
#define RK817_BUCK4_CMIN    0xC6
#define RK817_BOOST_CFG     0xDE
#define RK817_CHRG_STS      0xEB
#define RK817_SYS_STS       0xF0
#define RK817_SYS_STS_PLUG  0x40
#define RK817_SYS_CFG3      0xF4
#define RK817_DEV_OFF       0x01
#define RK817_ON_SOURCE     0xF5
#define RK817_OFF_SOURCE    0xF6
#define RK817_INT_STS0      0xF8
#define RK817_INT_PWRON_FALL  0x01
#define RK817_INT_PWRON_RISE  0x02
#define RK817_GPIO_INT_CFG  0xFE

typedef enum {
  Rk817KindNone,
  Rk817KindBuck,
  Rk817KindLdo,
  Rk817KindBoost,
  Rk817KindSwitch
} RK817_KIND;

typedef struct {
  RK817_KIND  Kind;
  UINT8       Index;       // 1-based DCDC/LDO number
  UINT8       VselReg;     // 0 when the rail has no voltage register
  UINT8       VselMask;
  UINT8       EnReg;
  UINT8       EnBit;       // bit in EnReg, write enable is EnBit + 4
} RK817_REGULATOR;

STATIC
BOOLEAN
Rk817IsChipId (
  IN UINT8  Msb,
  IN UINT8  Lsb
  )
{
  return (UINT16)((((UINT16)Msb << 8) | Lsb) & RK817_ID_MASK) == RK817_ID_VALUE;
}

STATIC
BOOLEAN
Rk817StrEq (
  IN CONST CHAR8  *A,
  IN CONST CHAR8  *B
  )
{
  while (*A != 0 && *A == *B) {
    A++;
    B++;
  }

  return *A == *B;
}

//
// Decode the DT node name of a regulator: DCDC_REG1..4, LDO_REG1..9, BOOST,
// OTG_SWITCH. The same names are in the kernel and U-Boot DTs.
//
STATIC
BOOLEAN
Rk817ParseRegulator (
  IN  CONST CHAR8     *Name,
  OUT RK817_REGULATOR *R
  )
{
  UINT8  N;

  R->Kind = Rk817KindNone;
  R->Index = 0;
  R->VselReg = 0;
  R->VselMask = 0;
  R->EnReg = 0;
  R->EnBit = 0;

  if (Name == NULL) {
    return FALSE;
  }

  if (Rk817StrEq (Name, "BOOST")) {
    R->Kind = Rk817KindBoost;
    R->VselReg = RK817_BOOST_CFG;
    R->VselMask = 0x07;
    R->EnReg = RK817_POWER_EN (3);
    R->EnBit = 1;
    return TRUE;
  }

  if (Rk817StrEq (Name, "OTG_SWITCH")) {
    R->Kind = Rk817KindSwitch;
    R->EnReg = RK817_POWER_EN (3);
    R->EnBit = 2;
    return TRUE;
  }

  if (Name[0] == 'D' && Name[1] == 'C' && Name[2] == 'D' && Name[3] == 'C' &&
      Name[4] == '_' && Name[5] == 'R' && Name[6] == 'E' && Name[7] == 'G' &&
      Name[8] >= '1' && Name[8] <= '4' && Name[9] == 0) {
    N = (UINT8)(Name[8] - '0');
    R->Kind = Rk817KindBuck;
    R->Index = N;
    R->VselReg = (UINT8)(0xBB + 3 * (N - 1));
    R->VselMask = 0x7F;
    R->EnReg = RK817_POWER_EN (0);
    R->EnBit = (UINT8)(N - 1);
    return TRUE;
  }

  if (Name[0] == 'L' && Name[1] == 'D' && Name[2] == 'O' && Name[3] == '_' &&
      Name[4] == 'R' && Name[5] == 'E' && Name[6] == 'G' &&
      Name[7] >= '1' && Name[7] <= '9' && Name[8] == 0) {
    N = (UINT8)(Name[7] - '0');
    R->Kind = Rk817KindLdo;
    R->Index = N;
    R->VselReg = (UINT8)(0xCC + 2 * (N - 1));
    R->VselMask = 0x7F;
    if (N <= 4) {
      R->EnReg = RK817_POWER_EN (1);
      R->EnBit = (UINT8)(N - 1);
    } else if (N <= 8) {
      R->EnReg = RK817_POWER_EN (2);
      R->EnBit = (UINT8)(N - 5);
    } else {
      R->EnReg = RK817_POWER_EN (3);
      R->EnBit = 0;
    }

    return TRUE;
  }

  return FALSE;
}

//
// Voltage select code for a rail, or FALSE when MicroVolts is out of range.
// DCDC1-3: 12.5 mV steps from 0.5 V up to 1.5 V (code 0..0x50), then 100 mV
// steps up to 2.4 V (0x59). DCDC4 goes on to 3.4 V (0x63). LDO: 25 mV from
// 0.6 V (code 0..0x70 = 3.4 V). BOOST: 100 mV from 4.7 V (code 0..7).
//
STATIC
BOOLEAN
Rk817VoltageToSel (
  IN  CONST RK817_REGULATOR  *R,
  IN  UINT32                 MicroVolts,
  OUT UINT8                  *Sel
  )
{
  UINT32  Max;

  switch (R->Kind) {
    case Rk817KindBuck:
      Max = (R->Index == 4) ? 3400000 : 2400000;
      if (MicroVolts < 500000 || MicroVolts > Max) {
        return FALSE;
      }

      if (MicroVolts <= 1500000) {
        *Sel = (UINT8)((MicroVolts - 500000) / 12500);
      } else {
        *Sel = (UINT8)(0x50 + (MicroVolts - 1500000) / 100000);
      }

      return TRUE;
    case Rk817KindLdo:
      if (MicroVolts < 600000 || MicroVolts > 3400000) {
        return FALSE;
      }

      *Sel = (UINT8)((MicroVolts - 600000) / 25000);
      return TRUE;
    case Rk817KindBoost:
      if (MicroVolts < 4700000 || MicroVolts > 5400000) {
        return FALSE;
      }

      *Sel = (UINT8)((MicroVolts - 4700000) / 100000);
      return TRUE;
    default:
      return FALSE;
  }
}

//
// The POWER_EN registers take a write enable in the upper nibble for every
// bit in the lower one, so a plain write changes only the named rail.
//
STATIC
UINT8
Rk817EnableWrite (
  IN UINT8    Bit,
  IN BOOLEAN  Enable
  )
{
  return (UINT8)((1U << (Bit + 4)) | (Enable ? (1U << Bit) : 0));
}

//
// Battery voltage in mV from the 16 bit ADC code. The ADC is calibrated at
// two points (2300 mV and 4025 mV) stored in VCALIB0/1 (fuel gauge driver).
//
STATIC
INT32
Rk817BatteryMilliVolts (
  IN UINT32  Raw,
  IN UINT32  Vcalib0,
  IN UINT32  Vcalib1
  )
{
  INT32  Delta;
  INT32  K;
  INT32  B;

  Delta = (INT32)Vcalib1 - (INT32)Vcalib0;
  if (Delta == 0) {
    Delta = 1;
  }

  K = (4025 - 2300) * 1000 / Delta;
  B = 4025 - (K * (INT32)Vcalib1) / 1000;
  return K * (INT32)Raw / 1000 + B;
}

//
// CHRG_STS bits 6:4: 0 off, 1 dead, 2 trickle, 3 CC/CV, 4 terminated.
//
STATIC
BOOLEAN
Rk817IsCharging (
  IN UINT8  ChrgSts
  )
{
  UINT8  State;

  State = (UINT8)((ChrgSts >> 4) & 0x7);
  return State >= 1 && State <= 3;
}

//
// The power key only shows up as latched fall/rise events. A fall means
// pressed until a rise is seen. A fall and a rise in one read is a short tap.
//
STATIC
BOOLEAN
Rk817PowerKeyState (
  IN BOOLEAN  WasPressed,
  IN UINT8    IntSts0
  )
{
  if ((IntSts0 & RK817_INT_PWRON_RISE) != 0) {
    return FALSE;
  }

  if ((IntSts0 & RK817_INT_PWRON_FALL) != 0) {
    return TRUE;
  }

  return WasPressed;
}

#endif

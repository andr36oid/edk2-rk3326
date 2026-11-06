/** @file
  Where PX30 keeps the pin configuration bits, as plain arithmetic so a host
  test can check it against the kernel's pinctrl tables. Source of truth:
  drivers/pinctrl/pinctrl-rockchip.c (px30_calc_*_reg_and_bit, rockchip_get_mux).

  GPIO0 is in the PMU GRF (2 mux bits per pin). GPIO1..3 are in the SYS GRF
  (4 mux bits per pin). Pull, drive and Schmitt use 2, 2 and 1 bit per pin.
  Only fixed-width types and the register base macros come from the includer.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _GPIO_REGS_H_
#define _GPIO_REGS_H_

#define GPIO_SWPORT_DR   0x00
#define GPIO_SWPORT_DDR  0x04
#define GPIO_EXT_PORT    0x50
#define GPIO_NGROUPS     4

typedef struct {
  UINTN   Reg;
  UINT32  Shift;
  UINT32  Mask;     // not yet shifted
} GPIO_REG_FIELD;

STATIC
VOID
GpioIomuxField (
  IN  UINT8           Group,
  IN  UINT8           Pin,
  OUT GPIO_REG_FIELD  *F
  )
{
  if (Group == 0) {
    F->Reg   = PMU_GRF + 0x00 + (Pin / 8) * 4;
    F->Shift = (Pin % 8) * 2;
    F->Mask  = 0x3;
  } else {
    F->Reg   = SYS_GRF + (Group - 1) * 0x20 + (Pin / 4) * 4;
    F->Shift = (Pin % 4) * 4;
    F->Mask  = 0xF;
  }
}

STATIC
VOID
GpioPullField (
  IN  UINT8           Group,
  IN  UINT8           Pin,
  OUT GPIO_REG_FIELD  *F
  )
{
  F->Reg   = (Group == 0 ? PMU_GRF + 0x10 : SYS_GRF + 0x60 + (Group - 1) * 0x10) + (Pin / 8) * 4;
  F->Shift = (Pin % 8) * 2;
  F->Mask  = 0x3;
}

STATIC
VOID
GpioDriveField (
  IN  UINT8           Group,
  IN  UINT8           Pin,
  OUT GPIO_REG_FIELD  *F
  )
{
  F->Reg   = (Group == 0 ? PMU_GRF + 0x20 : SYS_GRF + 0xF0 + (Group - 1) * 0x10) + (Pin / 8) * 4;
  F->Shift = (Pin % 8) * 2;
  F->Mask  = 0x3;
}

STATIC
VOID
GpioSchmittField (
  IN  UINT8           Group,
  IN  UINT8           Pin,
  OUT GPIO_REG_FIELD  *F
  )
{
  if (Group == 0) {
    F->Reg   = PMU_GRF + 0x38 + (Pin / 16) * 4;
    F->Shift = Pin % 16;
  } else {
    F->Reg   = SYS_GRF + 0xC0 + (Group - 1) * 0x10 + (Pin / 8) * 4;
    F->Shift = Pin % 8;
  }

  F->Mask = 0x1;
}

//
// The drive enum is a leftover from RK3568 where every value is a bit pattern;
// PX30 has four levels (2, 4, 8, 12 mA). Map the old names by strength.
//
STATIC
UINT8
GpioDriveEnumToLevel (
  IN UINT32  Drive
  )
{
  if (Drive <= 0x1) {
    return 0;
  } else if (Drive <= 0x3) {
    return 1;
  } else if (Drive <= 0xF) {
    return 2;
  }

  return 3;
}

#endif

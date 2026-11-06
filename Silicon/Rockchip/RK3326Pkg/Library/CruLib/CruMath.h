/** @file
  Clock register arithmetic that needs no hardware, so a host test can run it.
  Only fixed-width types come from the including file.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _CRU_MATH_H_
#define _CRU_MATH_H_

#define CRU_XIN24M_HZ     24000000U

//
// PX30 CLKSEL layout of the I2C clocks (kernel clk-px30.c): I2C0 and I2C1 share
// CLKSEL_CON49 (low and high byte), I2C2 and I2C3 share CLKSEL_CON50. In each
// byte bit 7 picks the source (1 = crystal) and bits 6:0 are divider - 1.
//
#define CRU_I2C_MUX_BIT   0x80U
#define CRU_I2C_DIV_MASK  0x7FU

typedef struct {
  UINT8  SelCon;
  UINT8  SelShift;
} CRU_I2C_CLOCK;

STATIC
CONST CRU_I2C_CLOCK *
CruI2cClock (
  IN UINT8  Bus
  )
{
  STATIC CONST CRU_I2C_CLOCK  Table[4] = {
    { 49, 0 }, { 49, 8 }, { 50, 0 }, { 50, 8 }
  };

  return &Table[Bus & 3];
}

//
// SARADC divider: CLKSEL_CON55 bits 10:0 hold divider - 1, rounded up so the
// ADC never runs faster than asked for.
//
STATIC
UINT32
CruSaradcDivField (
  IN UINT32  InputHz,
  IN UINT32  Hz
  )
{
  UINT32  Div;

  if (Hz == 0) {
    Hz = 1;
  }

  Div = (InputHz + Hz - 1) / Hz;
  if (Div < 1) {
    Div = 1;
  }

  if (Div > 0x800) {
    Div = 0x800;
  }

  return Div - 1;
}

#endif

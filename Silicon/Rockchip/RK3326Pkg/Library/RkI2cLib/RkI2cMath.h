/** @file
  RK3x I2C arithmetic: SCL divider and FIFO word packing. No hardware access.
  The includer provides UINT8/UINT32.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _RK_I2C_MATH_H_
#define _RK_I2C_MATH_H_

#define RK_I2C_FIFO_BYTES  32

//
// SCL = input clock / (8 * (CLKDIVL + CLKDIVH + 2)). The register holds
// divl in 15:0 and divh in 31:16. Rounds the rate down (slower is safe).
// ActualHz is optional.
//
STATIC
UINT32
RkI2cClkDiv (
  IN  UINT32  InputHz,
  IN  UINT32  SclHz,
  OUT UINT32  *ActualHz  OPTIONAL
  )
{
  UINT32  Div;
  UINT32  Divl;
  UINT32  Divh;

  if (SclHz == 0) {
    SclHz = 100000;
  }

  Div = (InputHz + SclHz * 8 - 1) / (SclHz * 8);
  Div = (Div > 2) ? Div - 2 : 0;
  Divl = Div / 2;
  Divh = Div - Divl;
  if (Divl > 0xFFFF) {
    Divl = 0xFFFF;
  }

  if (Divh > 0xFFFF) {
    Divh = 0xFFFF;
  }

  if (ActualHz != NULL) {
    *ActualHz = InputHz / (8 * (Divl + Divh + 2));
  }

  return (Divh << 16) | Divl;
}

//
// Pack bytes little endian into the 32-bit TXDATA words. Returns the number
// of words used.
//
STATIC
UINT32
RkI2cPackTx (
  IN  CONST UINT8  *Bytes,
  IN  UINT32       Count,
  OUT UINT32       Words[8]
  )
{
  UINT32  Index;
  UINT32  WordCount;

  WordCount = (Count + 3) / 4;
  for (Index = 0; Index < WordCount; Index++) {
    Words[Index] = 0;
  }

  for (Index = 0; Index < Count; Index++) {
    Words[Index / 4] |= (UINT32)Bytes[Index] << ((Index % 4) * 8);
  }

  return WordCount;
}

#endif

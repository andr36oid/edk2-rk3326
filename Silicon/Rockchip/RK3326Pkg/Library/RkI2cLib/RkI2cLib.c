/** @file
  RK3x I2C master (PX30), polled. Register use follows the Rockchip I2C
  driver in U-Boot (drivers/i2c/rk_i2c.c).

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/CruLib.h>
#include <Library/GpioLib.h>
#include <Library/RkI2cLib.h>
#include "RkI2cMath.h"

#define I2C_BASE(Bus)     (0xFF180000UL + (UINTN)(Bus) * 0x10000)
#define I2C_CON           0x00
#define I2C_CLKDIV        0x04
#define I2C_MRXADDR       0x08
#define I2C_MRXRADDR      0x0C
#define I2C_MTXCNT        0x10
#define I2C_MRXCNT        0x14
#define I2C_IEN           0x18
#define I2C_IPD           0x1C
#define I2C_TXDATA(n)     (0x100 + (n) * 4)
#define I2C_RXDATA(n)     (0x200 + (n) * 4)

#define CON_EN            BIT0
#define CON_MOD_TX        (0 << 1)
#define CON_MOD_TRX       (1 << 1)
#define CON_MOD_RX        (2 << 1)
#define CON_START         BIT3
#define CON_STOP          BIT4
#define CON_LASTACK       BIT5

#define INT_MBTF          BIT2
#define INT_MBRF          BIT3
#define INT_START         BIT4
#define INT_STOP          BIT5
#define INT_NAKRCV        BIT6
#define INT_ALL           0x7F

#define I2C_TIMEOUT_US    100000

STATIC UINT32  mInputHz[4];

//
// SCL/SDA pins (kernel px30.dtsi, no pull, Schmitt on): I2C0 GPIO0_B0/B1 and
// I2C1 GPIO0_C2/C3 are function 1. I2C0 is the PMIC bus, I2C1 has no device on
// any shipped board. I2C2/I2C3 are not wired up.
//
STATIC
EFI_STATUS
I2cSetPins (
  IN UINT8  Bus
  )
{
  switch (Bus) {
    case 0:
      GpioPinSetFunction (0, GPIO_PIN_PB0, 1);
      GpioPinSetFunction (0, GPIO_PIN_PB1, 1);
      GpioPinSetPull (0, GPIO_PIN_PB0, GPIO_PIN_PULL_NONE);
      GpioPinSetPull (0, GPIO_PIN_PB1, GPIO_PIN_PULL_NONE);
      GpioPinSetInput (0, GPIO_PIN_PB0, GPIO_PIN_INPUT_SCHMITT);
      GpioPinSetInput (0, GPIO_PIN_PB1, GPIO_PIN_INPUT_SCHMITT);
      return EFI_SUCCESS;
    case 1:
      GpioPinSetFunction (0, GPIO_PIN_PC2, 1);
      GpioPinSetFunction (0, GPIO_PIN_PC3, 1);
      GpioPinSetPull (0, GPIO_PIN_PC2, GPIO_PIN_PULL_NONE);
      GpioPinSetPull (0, GPIO_PIN_PC3, GPIO_PIN_PULL_NONE);
      GpioPinSetInput (0, GPIO_PIN_PC2, GPIO_PIN_INPUT_SCHMITT);
      GpioPinSetInput (0, GPIO_PIN_PC3, GPIO_PIN_INPUT_SCHMITT);
      return EFI_SUCCESS;
    default:
      return EFI_UNSUPPORTED;
  }
}

EFI_STATUS
EFIAPI
RkI2cInit (
  IN UINT8   Bus,
  IN UINT32  SclHz
  )
{
  EFI_STATUS  Status;
  UINT32      Actual;
  UINT32      Div;

  if (Bus > 3) {
    return EFI_INVALID_PARAMETER;
  }

  Status = I2cSetPins (Bus);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CruEnableGpioClocks ();
  mInputHz[Bus] = CruEnableI2cClock (Bus);

  Div = RkI2cClkDiv (mInputHz[Bus], SclHz, &Actual);
  MmioWrite32 (I2C_BASE (Bus) + I2C_CON, 0);
  MmioWrite32 (I2C_BASE (Bus) + I2C_CLKDIV, Div);

  DEBUG ((DEBUG_INFO, "I2C%u: input %u Hz, SCL ~%u Hz (CLKDIV 0x%08X)\n", Bus, mInputHz[Bus], Actual, Div));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
I2cWaitIpd (
  IN UINTN   Base,
  IN UINT32  Wanted,
  IN BOOLEAN FailOnNak
  )
{
  UINTN   Left;
  UINT32  Ipd;

  for (Left = I2C_TIMEOUT_US; Left > 0; Left--) {
    Ipd = MmioRead32 (Base + I2C_IPD);
    if (FailOnNak && ((Ipd & INT_NAKRCV) != 0)) {
      MmioWrite32 (Base + I2C_IPD, INT_NAKRCV);
      return EFI_NO_RESPONSE;
    }

    if ((Ipd & Wanted) != 0) {
      MmioWrite32 (Base + I2C_IPD, Wanted);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
I2cStart (
  IN UINTN  Base
  )
{
  MmioWrite32 (Base + I2C_IPD, INT_ALL);
  MmioWrite32 (Base + I2C_CON, CON_EN | CON_START);
  MmioWrite32 (Base + I2C_IEN, INT_START);
  return I2cWaitIpd (Base, INT_START, FALSE);
}

STATIC
VOID
I2cStop (
  IN UINTN  Base
  )
{
  MmioWrite32 (Base + I2C_IPD, INT_ALL);
  MmioWrite32 (Base + I2C_CON, CON_EN | CON_STOP);
  MmioWrite32 (Base + I2C_IEN, INT_STOP);
  I2cWaitIpd (Base, INT_STOP, FALSE);
  MmioWrite32 (Base + I2C_CON, 0);
}

EFI_STATUS
EFIAPI
RkI2cRead (
  IN  UINT8   Bus,
  IN  UINT8   Chip,
  IN  UINT8   Reg,
  OUT UINT8   *Buffer,
  IN  UINTN   Len
  )
{
  EFI_STATUS  Status;
  UINTN       Base;
  UINT32      Chunk;
  UINT32      Index;
  UINT32      Con;
  UINT32      Word = 0;
  BOOLEAN     Continued;

  if ((Bus > 3) || (Buffer == NULL) || (Len == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Base = I2C_BASE (Bus);
  Status = I2cStart (Base);
  if (EFI_ERROR (Status)) {
    I2cStop (Base);
    return Status;
  }

  //
  // Slave address (read), then the register address the controller sends
  // first in TRX mode: bit 24 marks the low byte valid.
  //
  MmioWrite32 (Base + I2C_MRXADDR, BIT24 | ((UINT32)Chip << 1) | 1);
  MmioWrite32 (Base + I2C_MRXRADDR, BIT24 | Reg);

  Continued = FALSE;
  while (Len > 0) {
    if (Len > RK_I2C_FIFO_BYTES) {
      Chunk = RK_I2C_FIFO_BYTES;
      Con   = CON_EN;
    } else {
      Chunk = (UINT32)Len;
      Con   = CON_EN | CON_LASTACK;
    }

    //
    // Only the first chunk carries the register address phase.
    //
    Con |= Continued ? CON_MOD_RX : CON_MOD_TRX;
    MmioWrite32 (Base + I2C_CON, Con);
    MmioWrite32 (Base + I2C_MRXCNT, Chunk);
    MmioWrite32 (Base + I2C_IEN, INT_MBRF | INT_NAKRCV);

    Status = I2cWaitIpd (Base, INT_MBRF, TRUE);
    if (EFI_ERROR (Status)) {
      break;
    }

    for (Index = 0; Index < Chunk; Index++) {
      if ((Index % 4) == 0) {
        Word = MmioRead32 (Base + I2C_RXDATA (Index / 4));
      }

      *Buffer++ = (UINT8)(Word >> ((Index % 4) * 8));
    }

    Len      -= Chunk;
    Continued = TRUE;
  }

  I2cStop (Base);
  return Status;
}

EFI_STATUS
EFIAPI
RkI2cWrite (
  IN UINT8        Bus,
  IN UINT8        Chip,
  IN UINT8        Reg,
  IN CONST UINT8  *Buffer,
  IN UINTN        Len
  )
{
  EFI_STATUS  Status;
  UINTN       Base;
  UINT8       Bytes[RK_I2C_FIFO_BYTES];
  UINT32      Words[8];
  UINT32      Count;
  UINT32      Index;
  UINT32      WordCount;

  //
  // One transfer: address byte, register byte, data. Longer than the FIFO is
  // not needed for the PMIC; refuse instead of splitting a register write.
  //
  if ((Bus > 3) || ((Buffer == NULL) && (Len != 0)) || (Len + 2 > RK_I2C_FIFO_BYTES)) {
    return EFI_INVALID_PARAMETER;
  }

  Count    = (UINT32)Len + 2;
  Bytes[0] = (UINT8)(Chip << 1);
  Bytes[1] = Reg;
  for (Index = 0; Index < Len; Index++) {
    Bytes[2 + Index] = Buffer[Index];
  }

  Base = I2C_BASE (Bus);
  Status = I2cStart (Base);
  if (!EFI_ERROR (Status)) {
    WordCount = RkI2cPackTx (Bytes, Count, Words);
    for (Index = 0; Index < WordCount; Index++) {
      MmioWrite32 (Base + I2C_TXDATA (Index), Words[Index]);
    }

    MmioWrite32 (Base + I2C_CON, CON_EN | CON_MOD_TX);
    MmioWrite32 (Base + I2C_MTXCNT, Count);
    MmioWrite32 (Base + I2C_IEN, INT_MBTF | INT_NAKRCV);
    Status = I2cWaitIpd (Base, INT_MBTF, TRUE);
  }

  I2cStop (Base);
  return Status;
}

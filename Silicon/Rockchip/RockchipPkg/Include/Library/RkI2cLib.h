/** @file
  Polled master driver for the Rockchip RK3x I2C controller. Pins, clocks and
  the divider are set here, nothing depends on what an earlier boot stage did.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _RK_I2C_LIB_H_
#define _RK_I2C_LIB_H_

#include <Uefi.h>

/**
  Pinmux, clocks and SCL rate (about SclHz) for bus Bus (0..3). Safe to call
  again.
**/
EFI_STATUS
EFIAPI
RkI2cInit (
  IN UINT8   Bus,
  IN UINT32  SclHz
  );

/**
  Read Len bytes starting at register Reg (one byte register address, repeated
  start). EFI_NO_RESPONSE when the chip does not ACK, EFI_TIMEOUT when the
  controller hangs.
**/
EFI_STATUS
EFIAPI
RkI2cRead (
  IN  UINT8   Bus,
  IN  UINT8   Chip,
  IN  UINT8   Reg,
  OUT UINT8   *Buffer,
  IN  UINTN   Len
  );

EFI_STATUS
EFIAPI
RkI2cWrite (
  IN UINT8        Bus,
  IN UINT8        Chip,
  IN UINT8        Reg,
  IN CONST UINT8  *Buffer,
  IN UINTN        Len
  );

#endif

// RkSaradcLib against U-Boot's drivers/adc/rockchip-saradc.c (px30 uses the
// rk3399 data: 10 bits, 1 MHz, data timeout SARADC_TIMEOUT / 5 = 20 ms):
// DLY_PU_SOC = 8, CTRL = power | irq enable | channel, wait for the status
// bit, DATA & 0x3FF, CTRL = 0. The ADC is a register model whose conversion
// ends some polls after the start.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent

#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/CruLib.h>
#include <Library/RkSaradcLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"

#define BASE       0xFF288000UL      // px30.dtsi saradc@ff288000
#define R_DATA     0x00
#define R_STAS     0x04
#define R_CTRL     0x08
#define R_DLY      0x0C
#define POWER      (1u << 3)          // SARADC_CTRL_POWER_CTRL
#define IRQ_EN     (1u << 5)          // SARADC_CTRL_IRQ_ENABLE
#define IRQ_STAT   (1u << 6)          // SARADC_CTRL_IRQ_STATUS

extern UINTN  mock_delay_us;

static UINT32  ctrl, dly, data;
static int     conv_polls;            // polls until the result is there, -1 never
static int     left;
static int     writes, clk_calls;
static UINT32  clk_hz;
static UINT32  log_reg[16], log_val[16];

VOID EFIAPI CruEnableSaradcClock (IN UINT32 Hz) { clk_calls++; clk_hz = Hz; }

UINT32 EFIAPI MmioRead32 (UINTN a)
{
  switch (a - BASE) {
    case R_DATA:
      return data;
    case R_CTRL:
      if ((ctrl & POWER) && (conv_polls >= 0) && (left-- <= 0)) {
        return ctrl | IRQ_STAT;
      }

      return ctrl;
  }

  printf ("FAIL: SARADC read 0x%lx\n", (unsigned long)a);
  exit (1);
}

UINT32 EFIAPI MmioWrite32 (UINTN a, UINT32 v)
{
  if (writes < 16) {
    log_reg[writes] = (UINT32)(a - BASE);
    log_val[writes] = v;
  }

  writes++;
  switch (a - BASE) {
    case R_CTRL:
      ctrl = v & ~IRQ_STAT;           // a write clears the status
      left = conv_polls;
      return v;
    case R_DLY:
      dly = v;
      return v;
  }

  printf ("FAIL: SARADC write 0x%x to 0x%lx\n", v, (unsigned long)a);
  exit (1);
}

static void Reset (int Polls, UINT32 Data)
{
  ctrl = 0; dly = 0; data = Data; conv_polls = Polls; left = 0;
  writes = 0; clk_calls = 0; clk_hz = 0; mock_delay_us = 0;
}

int main (void)
{
  UINT32  V, Ch;

  // Each channel: U-Boot's register sequence and the 10-bit result.
  for (Ch = 0; Ch <= 2; Ch++) {
    Reset (4, 0xFFFFFC00 | (0x155 + Ch));
    V = 0xDEAD;
    CHECK_EQ (RkSaradcRead ((UINT8)Ch, &V), EFI_SUCCESS);
    CHECK_EQ (V, 0x155 + Ch);
    CHECK_EQ (clk_calls, 1);
    CHECK_EQ (clk_hz, 1000000);
    CHECK_EQ (writes, 3);
    CHECK_EQ (log_reg[0], R_DLY);
    CHECK_EQ (log_val[0], 8);
    CHECK_EQ (log_reg[1], R_CTRL);
    CHECK_EQ (log_val[1], POWER | IRQ_EN | Ch);
    CHECK_EQ (log_reg[2], R_CTRL);
    CHECK_EQ (log_val[2], 0);                 // powered down again
    CHECK (mock_delay_us <= 10);
  }

  // Full scale and zero.
  Reset (0, 0x3FF);
  CHECK_EQ (RkSaradcRead (1, &V), EFI_SUCCESS);
  CHECK_EQ (V, 0x3FF);
  Reset (0, 0x400);
  CHECK_EQ (RkSaradcRead (1, &V), EFI_SUCCESS);
  CHECK_EQ (V, 0);

  // A slow conversion (15 ms) still counts; one that never ends gives up
  // after about U-Boot's 20 ms, powers the ADC down and leaves Value alone.
  Reset (15000, 0x123);
  CHECK_EQ (RkSaradcRead (2, &V), EFI_SUCCESS);
  CHECK_EQ (V, 0x123);
  Reset (-1, 0x123);
  V = 0xABCD;
  CHECK_EQ (RkSaradcRead (2, &V), EFI_TIMEOUT);
  CHECK_EQ (V, 0xABCD);
  CHECK (mock_delay_us >= 15000 && mock_delay_us <= 25000);
  CHECK_EQ (ctrl, 0);
  printf ("timeout after %lu us\n", (unsigned long)mock_delay_us);

  // Bad arguments touch nothing. (The PX30's SARADC has 6 inputs in U-Boot's
  // table; the library offers the 3 the header documents.)
  Reset (0, 0);
  CHECK_EQ (RkSaradcRead (3, &V), EFI_INVALID_PARAMETER);
  CHECK_EQ (RkSaradcRead (255, &V), EFI_INVALID_PARAMETER);
  CHECK_EQ (RkSaradcRead (0, NULL), EFI_INVALID_PARAMETER);
  CHECK_EQ (writes, 0);
  CHECK_EQ (clk_calls, 0);

  DONE ();
}

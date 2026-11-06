/** @file
 *
 *  Copyright (c) 2021, Jared McNeill <jmcneill@invisible.ca>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#ifndef CRULIB_H__
#define CRULIB_H__

VOID
CruAssertSoftReset (
  IN UINT32 Index,
  IN UINT8 Bit
  );


VOID
CruDeassertSoftReset (
  IN UINT32 Index,
  IN UINT8 Bit
  );

UINTN
CruGetSdmmcClockRate (
  IN UINT8 Index
  );

VOID
CruSetSdmmcClockRate (
  IN UINT8 Index,
  IN UINTN Rate
  );

//
// PX30 peripheral clocks the boards need before U-Boot-independent bring-up.
// All of them run from the 24 MHz crystal so nothing here depends on how a
// previous boot stage left the PLLs.
//

/**
  Ungate the GPIO bank clocks (GPIO0 sits in the PMU domain).
**/
VOID
CruEnableGpioClocks (
  VOID
  );

/**
  Clock I2C bus Bus (0..3) from the 24 MHz crystal, ungate its pclk.
  Returns the input clock of the controller in Hz (for the SCL divider).
**/
UINT32
CruEnableI2cClock (
  IN UINT8  Bus
  );

/**
  Clock the SARADC from the crystal divided down to about Hz and ungate its pclk.
**/
VOID
CruEnableSaradcClock (
  IN UINT32  Hz
  );

#define CRU_BASE            0xff2B0000UL
#define PMUCRU_BASE         0xff2BC000UL

/* GATE registers (a set bit stops the clock) */
#define CRU_CLKGATE_CON(n)  (CRU_BASE + (n) * 0x4 + 0x200)
#define PMUCRU_CLKGATE_CON(n)  (PMUCRU_BASE + (n) * 0x4 + 0x80)

/* CLKSEL registers */
#define CRU_CLKSEL_CON(n)   (CRU_BASE + (n) * 0x4 + 0x100)

#define CRU_CLKSEL_CON16_CLK_SDMMC_SEL_SHIFT     14
#define CRU_CLKSEL_CON16_CLK_SDMMC_SEL_MASK      (0x3U << CRU_CLKSEL_CON17_CLK_SDMMC_SEL_SHIFT)


/* CLKSEL_CON17 fields */
// From CRU_CLKSEL_CON17
#define CRU_CLKSEL_CON17_CLK_SDMMC_SEL_SHIFT     15
#define CRU_CLKSEL_CON17_CLK_SDMMC_SEL_MASK      (0x1U << CRU_CLKSEL_CON17_CLK_SDMMC_SEL_SHIFT)

/* SOFTRST registers */
#define CRU_SOFTRST_CON(n)  (CRU_BASE + (n) * 0x4 + 0x300)

//
// Display clocks (PX30 VOPB -> MIPI DSI, backlight PWM), for RkDisplayDxe.
//

typedef enum {
  CruPllApll = 0,
  CruPllDpll,
  CruPllCpll,
  CruPllNpll,
  CruPllGpll          // in the PMU CRU
} CRU_PLL;

/**
  Output rate of a PLL in Hz, from its registers (24 MHz in slow mode).
**/
UINT64
EFIAPI
CruGetPllRate (
  IN CRU_PLL  Pll
  );

/**
  Ungate the VO bus, VOPB, DSI host and DSI PHY clocks and select the
  24 MHz crystal as the DSI PHY reference.
**/
VOID
EFIAPI
CruEnableDisplayClocks (
  VOID
  );

/**
  Set DCLK_VOPB to Hz the way U-Boot does: CPLL retuned to Hz times an even
  divider near 600 MHz. Returns the rate read back, 0 on failure.
**/
UINT64
EFIAPI
CruSetVopbDclk (
  IN UINT32  Hz
  );

/**
  Ungate PWM block Block (0: pwm0-3 at 0xFF200000, 1: pwm4-7 at 0xFF208000)
  and return its counter clock in Hz (0 for a bad Block).
**/
UINT64
EFIAPI
CruEnablePwmClock (
  IN UINT8  Block
  );

#endif /* CRULIB_H__ */

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

#define CRU_BASE            0xff2B0000UL

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

#endif /* CRULIB_H__ */

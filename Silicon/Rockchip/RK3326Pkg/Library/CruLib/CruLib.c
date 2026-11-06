/** @file
 *
 *  RK3566/RK3568 Clock and Reset Unit Library.
 * 
 *  Copyright (c) 2021, Jared McNeill <jmcneill@invisible.ca>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/CruLib.h>
#include "CruMath.h"


VOID
CruAssertSoftReset (
  IN UINT32 Index,
  IN UINT8 Bit
  )
{
    MmioWrite32 (CRU_SOFTRST_CON (Index), (1U << (Bit + 16)) | (1U << Bit));
}

VOID
CruDeassertSoftReset (
  IN UINT32 Index,
  IN UINT8 Bit
  )
{
    MmioWrite32 (CRU_SOFTRST_CON (Index), 1U << (Bit + 16));
}

//
// SD (index 0) and eMMC (index 1) clocks, as U-Boot's PX30 clock driver sets
// them. The controller halves the CRU clock (its internal clkgen), so the CRU
// runs at twice the rate the DW-MMC divider starts from ("cclk_in", which is
// what these functions take and return). Source GPLL, or the 24 MHz crystal
// when GPLL can't be divided down far enough.
//
#define PMUCRU_GPLL_CON0      (PMUCRU_BASE + 0x00)
#define PMUCRU_GPLL_CON1      (PMUCRU_BASE + 0x04)
#define PMUCRU_MODE           (PMUCRU_BASE + 0x20)
#define MMC_SRC_GPLL          0
#define MMC_SRC_24M           3
#define MMC_SRC_SHIFT         14
#define MMC_DIV_MASK          0xFF
#define MMC_DIV50_SEL         BIT15     // in CLKSEL_CON(n + 1): 0 = clk_*_div

STATIC CONST struct {
  UINT8    ClkSel;        // CLKSEL_CON: source (15:14) and divider (7:0)
  UINT8    Gates[6][2];   // { CLKGATE_CON, bit } pairs to open, bus clocks first
} mMmcClk[2] = {
  // SDMMC: hclk_sdmmc_pre, hclk_sdmmc_niu, hclk_sdmmc, clk_sdmmc_div, _div50, clk_sdmmc
  { 16, { { 6, 12 }, { 7, 0 }, { 7, 1 }, { 6, 13 }, { 6, 14 }, { 6, 15 } } },
  // EMMC: hclk_mmc_nand, hclk_emmc, clk_emmc_div, _div50, clk_emmc (last entry repeats)
  { 20, { { 6, 0 }, { 6, 10 }, { 6, 4 }, { 6, 5 }, { 6, 6 }, { 6, 6 } } },
};

UINTN
CruGetGpllRate (
  VOID
  )
{
  UINT32  Con0, Con1;
  UINT32  FbDiv, RefDiv, PostDiv1, PostDiv2;

  // PMUCRU mode bits 1:0: 0 = 24 MHz bypass, 1 = PLL output.
  if ((MmioRead32 (PMUCRU_MODE) & 0x3) != 1) {
    return 24000000U;
  }

  Con0     = MmioRead32 (PMUCRU_GPLL_CON0);
  Con1     = MmioRead32 (PMUCRU_GPLL_CON1);
  FbDiv    = Con0 & 0xFFF;
  PostDiv1 = (Con0 >> 12) & 0x7;
  RefDiv   = Con1 & 0x3F;
  PostDiv2 = (Con1 >> 6) & 0x7;
  if ((RefDiv == 0) || (PostDiv1 == 0) || (PostDiv2 == 0)) {
    return 0;
  }

  return (UINTN)(24 * FbDiv / (RefDiv * PostDiv1 * PostDiv2)) * 1000000U;
}

UINTN
CruGetSdmmcClockRate (
  IN UINT8  Index
  )
{
  UINT32  Con;
  UINTN   Src;

  if (Index > 1) {
    return 0;
  }

  Con = MmioRead32 (CRU_CLKSEL_CON (mMmcClk[Index].ClkSel));
  Src = (((Con >> MMC_SRC_SHIFT) & 0x3) == MMC_SRC_24M) ? 24000000U : CruGetGpllRate ();
  return Src / ((Con & MMC_DIV_MASK) + 1) / 2;
}

VOID
CruSetSdmmcClockRate (
  IN UINT8  Index,
  IN UINTN  Rate
  )
{
  UINTN   Gpll;
  UINTN   Src;
  UINT32  SrcSel;
  UINTN   Div;
  UINTN   Bit;

  if ((Index > 1) || (Rate == 0)) {
    return;
  }

  Gpll   = CruGetGpllRate ();
  Src    = Gpll;
  SrcSel = MMC_SRC_GPLL;
  Div    = (Gpll != 0) ? (Gpll / 2 + Rate - 1) / Rate : 0;
  if ((Div == 0) || (Div > 128)) {
    Src    = 24000000U;
    SrcSel = MMC_SRC_24M;
    Div    = (Src / 2 + Rate - 1) / Rate;
    if (Div > 128) {
      Div = 128;
    }
  }

  MmioWrite32 (
    CRU_CLKSEL_CON (mMmcClk[Index].ClkSel),
    ((0x3U << MMC_SRC_SHIFT | MMC_DIV_MASK) << 16) | (SrcSel << MMC_SRC_SHIFT) | (UINT32)(Div - 1)
    );
  // Take clk_*_div, not the div50 path.
  MmioWrite32 (CRU_CLKSEL_CON (mMmcClk[Index].ClkSel + 1), (UINT32)MMC_DIV50_SEL << 16);
  // Ungate: hiword-masked, a written 0 enables.
  for (Bit = 0; Bit < ARRAY_SIZE (mMmcClk[Index].Gates); Bit++) {
    MmioWrite32 (CRU_CLKGATE_CON (mMmcClk[Index].Gates[Bit][0]), 1U << (mMmcClk[Index].Gates[Bit][1] + 16));
  }

  DEBUG ((
    DEBUG_INFO,
    "CRU: mmc%u from %a %lu Hz / %lu / 2 = %lu Hz (asked %lu)\n",
    Index,
    (SrcSel == MMC_SRC_24M) ? "24M" : "GPLL",
    (UINT64)Src,
    (UINT64)Div,
    (UINT64)CruGetSdmmcClockRate (Index),
    (UINT64)Rate
    ));
}

//
// Gate registers take a write mask in the upper half: writing only the mask
// bit clears the gate (clock on).
//
STATIC
VOID
CruUngate (
  IN UINTN   Reg,
  IN UINT32  Bits
  )
{
  MmioWrite32 (Reg, Bits << 16);
}

VOID
CruEnableGpioClocks (
  VOID
  )
{
  CruUngate (CRU_CLKGATE_CON (15), (1U << 8) | (1U << 9) | (1U << 10));
  CruUngate (PMUCRU_CLKGATE_CON (0), 1U << 6);
}

UINT32
CruEnableI2cClock (
  IN UINT8  Bus
  )
{
  CONST CRU_I2C_CLOCK  *Clk;

  ASSERT (Bus < 4);
  Clk = CruI2cClock (Bus);

  //
  // Mux to the crystal (bit = 1) and divide by one (field = 0).
  //
  MmioWrite32 (
    CRU_CLKSEL_CON (Clk->SelCon),
    ((CRU_I2C_MUX_BIT | CRU_I2C_DIV_MASK) << Clk->SelShift << 16) |
    (CRU_I2C_MUX_BIT << Clk->SelShift)
    );
  CruUngate (CRU_CLKGATE_CON (12), 1U << Bus);
  CruUngate (CRU_CLKGATE_CON (14), 1U << (10 + Bus));

  return CRU_XIN24M_HZ;
}

VOID
CruEnableSaradcClock (
  IN UINT32  Hz
  )
{
  UINT32  Div;

  Div = CruSaradcDivField (CRU_XIN24M_HZ, Hz);
  MmioWrite32 (CRU_CLKSEL_CON (55), (0x7FFU << 16) | Div);
  CruUngate (CRU_CLKGATE_CON (12), 1U << 10);
  CruUngate (CRU_CLKGATE_CON (15), 1U << 3);
}

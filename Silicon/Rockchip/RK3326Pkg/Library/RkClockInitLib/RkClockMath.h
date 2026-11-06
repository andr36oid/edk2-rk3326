/** @file
  PX30 PLL and bus clock arithmetic, ported from U-Boot drivers/clk/rockchip/
  clk_px30.c and cru_px30.h. No hardware access, so a host test can run it.
  The includer provides the fixed-width types, STATIC, CONST and NULL.

  SPDX-License-Identifier: GPL-2.0+
**/

#ifndef _RK_CLOCK_MATH_H_
#define _RK_CLOCK_MATH_H_

#define RKCLK_OSC_HZ    24000000U
#define RKCLK_APLL_HZ   600000000U     // cru_px30.h APLL_HZ
#define RKCLK_GPLL_HZ   1200000000U
#define RKCLK_ACLK_BUS_HZ   200000000U
#define RKCLK_HCLK_BUS_HZ   150000000U
#define RKCLK_PCLK_BUS_HZ   100000000U
#define RKCLK_ACLK_PERI_HZ  200000000U
#define RKCLK_HCLK_PERI_HZ  150000000U

//
// PLL_CON0: FBDIV [11:0], POSTDIV1 [14:12]. PLL_CON1: REFDIV [5:0],
// POSTDIV2 [8:6], DSMPD BIT12, PD BIT13, lock status BIT10.
//
#define RKCLK_PLL_FBDIV_MASK     0x0FFFU
#define RKCLK_PLL_POSTDIV1_SHIFT 12
#define RKCLK_PLL_POSTDIV1_MASK  (7U << RKCLK_PLL_POSTDIV1_SHIFT)
#define RKCLK_PLL_REFDIV_MASK    0x3FU
#define RKCLK_PLL_POSTDIV2_SHIFT 6
#define RKCLK_PLL_POSTDIV2_MASK  (7U << RKCLK_PLL_POSTDIV2_SHIFT)
#define RKCLK_PLL_DSMPD          (1U << 12)
#define RKCLK_PLL_PD             (1U << 13)
#define RKCLK_PLL_LOCK           (1U << 10)

// Mode register: 2 bits per PLL, 0 = 24 MHz crystal (slow), 1 = PLL output.
#define RKCLK_MODE_MASK          3U
#define RKCLK_MODE_SLOW          0U
#define RKCLK_MODE_PLL           1U

typedef struct {
  UINT32  Rate;
  UINT32  RefDiv;
  UINT32  FbDiv;
  UINT32  PostDiv1;
  UINT32  PostDiv2;
} RKCLK_PLL_PARAMS;

// The rows of px30_pll_rates[] this boot needs (integer mode, DSMPD = 1).
STATIC CONST RKCLK_PLL_PARAMS  mRkClkPllRates[] = {
  { 1200000000U, 1, 50, 1, 1 },
  { 600000000U,  1, 75, 3, 1 },
};

STATIC
CONST RKCLK_PLL_PARAMS *
RkClkPllParams (
  IN UINT32  Rate
  )
{
  UINTN  I;

  for (I = 0; I < sizeof (mRkClkPllRates) / sizeof (mRkClkPllRates[0]); I++) {
    if (mRkClkPllRates[I].Rate == Rate) {
      return &mRkClkPllRates[I];
    }
  }

  return NULL;
}

//
// FOUTPOSTDIV = 24 MHz / REFDIV * FBDIV / POSTDIV1 / POSTDIV2 (integer mode).
// Same integer steps as U-Boot's rkclk_pll_get_rate: MHz first, then times 1e6.
//
STATIC
UINT32
RkClkPllHz (
  IN UINT32  Con0,
  IN UINT32  Con1
  )
{
  UINT32  Fb = Con0 & RKCLK_PLL_FBDIV_MASK;
  UINT32  P1 = (Con0 & RKCLK_PLL_POSTDIV1_MASK) >> RKCLK_PLL_POSTDIV1_SHIFT;
  UINT32  P2 = (Con1 & RKCLK_PLL_POSTDIV2_MASK) >> RKCLK_PLL_POSTDIV2_SHIFT;
  UINT32  Rd = Con1 & RKCLK_PLL_REFDIV_MASK;

  if ((Rd == 0) || (P1 == 0) || (P2 == 0)) {
    return 0;
  }

  return (24 * Fb / (Rd * P1 * P2)) * 1000000U;
}

// DIV_ROUND_UP(Parent, Target) - 1, the value the divider fields hold.
STATIC
UINT32
RkClkDivField (
  IN UINT32  Parent,
  IN UINT32  Target,
  IN UINT32  MaxField
  )
{
  UINT32  Div = (Parent + Target - 1) / Target;

  Div = (Div == 0) ? 0 : Div - 1;
  return (Div > MaxField) ? MaxField : Div;
}

// CLKSEL_CON0 (core): ACLK div [14:12], DBG div [9:8], source BIT7, div [3:0].
typedef struct {
  UINT32  Rate;
  UINT32  AclkDiv;
  UINT32  PclkDiv;
} RKCLK_CPU_PARAMS;

// px30_cpu_rates[], the row for 600 MHz.
STATIC CONST RKCLK_CPU_PARAMS  mRkClkCpuRates[] = {
  { 600000000U, 1, 3 },
};

#define RKCLK_CORE_MASK   ((7U << 12) | (3U << 8) | (1U << 7) | 0xFU)
#define RKCLK_CORE_VALUE(Aclk, Dbg)  (((Aclk) << 12) | ((Dbg) << 8))   // source APLL, div 0

//
// Bus and peripheral dividers. CLKSEL_CON23: source BIT15 (0 = GPLL), ACLK_BUS
// div [12:8]. CON24: HCLK_BUS div [4:0], PCLK_BUS div [9:8] (off ACLK_BUS).
// CON14: source BIT15, ACLK_PERI div [4:0], HCLK_PERI div [12:8].
//
typedef struct {
  UINT32  Con23;      // masks are fixed: BIT15 | 0x1F << 8
  UINT32  Con24;      // 0x1F | 3 << 8
  UINT32  Con14;      // BIT15 | 0x1F | 0x1F << 8
} RKCLK_BUS_DIVS;

#define RKCLK_CON23_MASK  ((1U << 15) | (0x1FU << 8))
#define RKCLK_CON24_MASK  (0x1FU | (3U << 8))
#define RKCLK_CON14_MASK  ((1U << 15) | 0x1FU | (0x1FU << 8))

STATIC
VOID
RkClkBusDivs (
  IN  UINT32          Gpll,
  OUT RKCLK_BUS_DIVS  *D
  )
{
  UINT32  AclkDiv  = RkClkDivField (Gpll, RKCLK_ACLK_BUS_HZ, 31);
  UINT32  AclkRate = Gpll / (AclkDiv + 1);

  D->Con23 = AclkDiv << 8;
  D->Con24 = RkClkDivField (Gpll, RKCLK_HCLK_BUS_HZ, 31) |
             (RkClkDivField (AclkRate, RKCLK_PCLK_BUS_HZ, 3) << 8);
  D->Con14 = RkClkDivField (Gpll, RKCLK_ACLK_PERI_HZ, 31) |
             (RkClkDivField (Gpll, RKCLK_HCLK_PERI_HZ, 31) << 8);
}

#endif

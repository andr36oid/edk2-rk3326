/** @file
  PX30 PLL and VOP dclk divider arithmetic, kept free of register access so
  the host test can compare it with U-Boot.

  Ported from U-Boot drivers/clk/rockchip/clk_px30.c (px30_pll_rates,
  pll_clk_set_by_auto, px30_vop_set_clk). Same choices, so the CPLL ends up
  where U-Boot puts it for the same pixel clock.

  SPDX-License-Identifier: GPL-2.0+
**/

#ifndef CRU_PLL_H_
#define CRU_PLL_H_

#define CRU_PLL_OSC_HZ         24000000U
#define CRU_PLL_VCO_MIN_KHZ    800000U
#define CRU_PLL_VCO_MAX_KHZ    3200000U
#define CRU_VOP_PLL_LIMIT_HZ   600000000U

typedef struct {
  UINT32  RefDiv;
  UINT32  FbDiv;
  UINT32  PostDiv1;
  UINT32  PostDiv2;
} CRU_PLL_SETTINGS;

typedef struct {
  UINT32              Rate;
  CRU_PLL_SETTINGS    Pll;
} CRU_PLL_TABLE_ENTRY;

//
// U-Boot px30_pll_rates: exact rates use these settings, not the search.
//
STATIC CONST CRU_PLL_TABLE_ENTRY  mCruPllTable[] = {
  { 1200000000U, { 1,  50,  1, 1 } },
  { 1188000000U, { 2,  99,  1, 1 } },
  { 1100000000U, { 12, 550, 1, 1 } },
  { 1008000000U, { 1,  84,  2, 1 } },
  { 1000000000U, { 6,  500, 2, 1 } },
  { 816000000U,  { 1,  68,  2, 1 } },
  { 600000000U,  { 1,  75,  3, 1 } },
};

/**
  Integer-mode settings for Rate Hz. FALSE when no setting within 4 MHz
  exists (U-Boot refuses those too).
**/
STATIC
BOOLEAN
CruPllFindSettings (
  IN  UINT32            Rate,
  OUT CRU_PLL_SETTINGS  *Out
  )
{
  UINT32  Index;
  UINT32  RefKhz;
  UINT32  RateKhz;
  UINT32  PostDiv1;
  UINT32  PostDiv2;
  UINT32  VcoKhz;
  UINT32  RefDiv;
  UINT32  FrefKhz;
  UINT32  FbDiv;
  UINT32  DiffKhz;
  UINT32  BestDiffKhz;

  for (Index = 0; Index < sizeof (mCruPllTable) / sizeof (mCruPllTable[0]); Index++) {
    if (mCruPllTable[Index].Rate == Rate) {
      *Out = mCruPllTable[Index].Pll;
      return TRUE;
    }
  }

  RateKhz = Rate / 1000;
  if (RateKhz == 0) {
    return FALSE;
  }

  RefKhz   = CRU_PLL_OSC_HZ / 1000;
  PostDiv2 = 1;
  PostDiv1 = (CRU_PLL_VCO_MIN_KHZ + RateKhz - 1) / RateKhz;
  if (PostDiv1 > 7) {
    PostDiv2 = (PostDiv1 + 6) / 7;
    PostDiv1 = (PostDiv1 + PostDiv2 - 1) / PostDiv2;
  }

  VcoKhz = RateKhz * PostDiv1 * PostDiv2;
  if ((VcoKhz < CRU_PLL_VCO_MIN_KHZ) || (VcoKhz > CRU_PLL_VCO_MAX_KHZ) || (PostDiv2 > 7)) {
    return FALSE;
  }

  Out->PostDiv1 = PostDiv1;
  Out->PostDiv2 = PostDiv2;
  Out->RefDiv   = 0;
  Out->FbDiv    = 0;

  BestDiffKhz = VcoKhz;
  for (RefDiv = 1; (RefDiv < 63) && (BestDiffKhz != 0); RefDiv++) {
    FrefKhz = RefKhz / RefDiv;
    FbDiv   = VcoKhz / FrefKhz;
    if ((FbDiv >= 3200) || (FbDiv <= 16)) {
      continue;
    }

    DiffKhz = VcoKhz - FbDiv * FrefKhz;
    if ((FbDiv + 1 < 3200) && (DiffKhz > FrefKhz / 2)) {
      FbDiv++;
      DiffKhz = FrefKhz - DiffKhz;
    }

    if (DiffKhz >= BestDiffKhz) {
      continue;
    }

    BestDiffKhz  = DiffKhz;
    Out->RefDiv  = RefDiv;
    Out->FbDiv   = FbDiv;
  }

  return (BestDiffKhz <= 4000) && (Out->RefDiv != 0);
}

/**
  Output rate in Hz of integer-mode settings.
**/
STATIC
UINT64
CruPllRate (
  IN CONST CRU_PLL_SETTINGS  *Pll
  )
{
  UINT64  Div;

  Div = (UINT64)Pll->RefDiv * Pll->PostDiv1 * Pll->PostDiv2;
  if (Div == 0) {
    return 0;
  }

  return ((UINT64)CRU_PLL_OSC_HZ * Pll->FbDiv) / Div;
}

/**
  DCLK_VOPB divider for Hz, as px30_vop_set_clk picks it: 600 MHz / Hz
  rounded up, then down to an even number. CPLL then runs at Hz * divider.
**/
STATIC
UINT32
CruVopDclkDivider (
  IN UINT32  Hz
  )
{
  UINT32  Div;

  if ((Hz == 0) || (Hz >= CRU_VOP_PLL_LIMIT_HZ)) {
    return 1;
  }

  Div = (CRU_VOP_PLL_LIMIT_HZ + Hz - 1) / Hz;
  if ((Div % 2) != 0) {
    Div--;
  }

  return Div;
}

#endif

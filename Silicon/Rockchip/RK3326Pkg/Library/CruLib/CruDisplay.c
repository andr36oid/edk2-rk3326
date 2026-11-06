/** @file
  PX30 clocks for the VOPB -> MIPI DSI path and the backlight PWM.

  Ported from U-Boot drivers/clk/rockchip/clk_px30.c (rkclk_set_pll,
  rkclk_pll_get_rate, px30_vop_set_clk, px30_pwm_get_clk); gate bits from
  the kernel's clk-px30.c. Register names here are local so they cannot
  clash with what other CruLib users add to CruLib.h.

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/CruLib.h>
#include "CruPll.h"

#define DISP_CRU_BASE        0xFF2B0000UL
#define DISP_PMUCRU_BASE     0xFF2BC000UL

#define DISP_CRU_PLL_CON(Pll, n)   (DISP_CRU_BASE + (Pll) * 0x20 + (n) * 4)
#define DISP_CRU_MODE              (DISP_CRU_BASE + 0xA0)
#define DISP_CRU_CLKSEL(n)         (DISP_CRU_BASE + 0x100 + (n) * 4)
#define DISP_CRU_GATE(n)           (DISP_CRU_BASE + 0x200 + (n) * 4)
#define DISP_PMUCRU_PLL_CON(n)     (DISP_PMUCRU_BASE + (n) * 4)
#define DISP_PMUCRU_MODE           (DISP_PMUCRU_BASE + 0x20)
#define DISP_PMUCRU_CLKSEL(n)      (DISP_PMUCRU_BASE + 0x40 + (n) * 4)
#define DISP_PMUCRU_GATE(n)        (DISP_PMUCRU_BASE + 0x80 + (n) * 4)

// CRU registers take a write mask in the upper half.
#define HIWORD(Mask, Value)  (((UINT32)(Mask) << 16) | ((Value) & (Mask)))

#define PLL_CON0_POSTDIV1_SHIFT  12
#define PLL_CON0_POSTDIV1_MASK   (7U << 12)
#define PLL_CON0_FBDIV_MASK      0xFFFU
#define PLL_CON1_PD              BIT13
#define PLL_CON1_DSMPD           BIT12
#define PLL_CON1_LOCK            BIT10
#define PLL_CON1_POSTDIV2_SHIFT  6
#define PLL_CON1_POSTDIV2_MASK   (7U << 6)
#define PLL_CON1_REFDIV_MASK     0x3FU

#define PLL_MODE_SLOW            0   // xin24m
#define PLL_MODE_NORMAL          1

// CLKSEL_CON5: dclk_vopb_src divider, CPLL/NPLL choice, divout/frac/24M mux
#define DCLK_VOPB_SEL_MASK       (3U << 14)
#define DCLK_VOPB_PLL_SEL_MASK   BIT11
#define DCLK_VOPB_DIV_MASK       0xFFU

STATIC CONST CHAR8  *mPllNames[] = { "APLL", "DPLL", "CPLL", "NPLL", "GPLL" };

UINT64
EFIAPI
CruGetPllRate (
  IN CRU_PLL  Pll
  )
{
  UINT32            Mode;
  UINT32            Con0;
  UINT32            Con1;
  UINT32            Frac;
  CRU_PLL_SETTINGS  Settings;
  UINT64            Rate;

  if (Pll == CruPllGpll) {
    Mode = MmioRead32 (DISP_PMUCRU_MODE) & 3;
    Con0 = MmioRead32 (DISP_PMUCRU_PLL_CON (0));
    Con1 = MmioRead32 (DISP_PMUCRU_PLL_CON (1));
    Frac = MmioRead32 (DISP_PMUCRU_PLL_CON (2)) & 0xFFFFFF;
  } else if (Pll <= CruPllNpll) {
    Mode = (MmioRead32 (DISP_CRU_MODE) >> (Pll * 2)) & 3;
    Con0 = MmioRead32 (DISP_CRU_PLL_CON (Pll, 0));
    Con1 = MmioRead32 (DISP_CRU_PLL_CON (Pll, 1));
    Frac = MmioRead32 (DISP_CRU_PLL_CON (Pll, 2)) & 0xFFFFFF;
  } else {
    return 0;
  }

  if (Mode == PLL_MODE_SLOW) {
    return CRU_PLL_OSC_HZ;
  }

  if (Mode != PLL_MODE_NORMAL) {
    return 32768;
  }

  Settings.FbDiv    = Con0 & PLL_CON0_FBDIV_MASK;
  Settings.PostDiv1 = (Con0 & PLL_CON0_POSTDIV1_MASK) >> PLL_CON0_POSTDIV1_SHIFT;
  Settings.PostDiv2 = (Con1 & PLL_CON1_POSTDIV2_MASK) >> PLL_CON1_POSTDIV2_SHIFT;
  Settings.RefDiv   = Con1 & PLL_CON1_REFDIV_MASK;
  Rate              = CruPllRate (&Settings);

  // Fractional mode adds Frac / 2^24 to FBDIV.
  if (((Con1 & PLL_CON1_DSMPD) == 0) && (Frac != 0) && (Settings.RefDiv * Settings.PostDiv1 * Settings.PostDiv2 != 0)) {
    Rate += ((UINT64)CRU_PLL_OSC_HZ * Frac / Settings.RefDiv / Settings.PostDiv1 / Settings.PostDiv2) >> 24;
  }

  return Rate;
}

/**
  rkclk_set_pll for one of the CRU PLLs: slow mode, integer mode, power down,
  new dividers, power up, wait for lock, normal mode.
**/
STATIC
EFI_STATUS
CruSetPll (
  IN CRU_PLL                 Pll,
  IN CONST CRU_PLL_SETTINGS  *Settings
  )
{
  UINTN   Con0;
  UINTN   Con1;
  UINT32  Shift;
  UINTN   Timeout;

  Con0  = DISP_CRU_PLL_CON (Pll, 0);
  Con1  = DISP_CRU_PLL_CON (Pll, 1);
  Shift = Pll * 2;

  MmioWrite32 (DISP_CRU_MODE, HIWORD (3U << Shift, PLL_MODE_SLOW << Shift));
  MmioWrite32 (Con1, HIWORD (PLL_CON1_DSMPD, PLL_CON1_DSMPD));
  MmioWrite32 (Con1, HIWORD (PLL_CON1_PD, PLL_CON1_PD));
  MmioWrite32 (
    Con0,
    HIWORD (
      PLL_CON0_POSTDIV1_MASK | PLL_CON0_FBDIV_MASK,
      (Settings->PostDiv1 << PLL_CON0_POSTDIV1_SHIFT) | Settings->FbDiv
      )
    );
  MmioWrite32 (
    Con1,
    HIWORD (
      PLL_CON1_POSTDIV2_MASK | PLL_CON1_REFDIV_MASK,
      (Settings->PostDiv2 << PLL_CON1_POSTDIV2_SHIFT) | Settings->RefDiv
      )
    );
  MmioWrite32 (Con1, HIWORD (PLL_CON1_PD, 0));

  // U-Boot waits forever here; a PLL that never locks stays in slow mode.
  for (Timeout = 0; (MmioRead32 (Con1) & PLL_CON1_LOCK) == 0; Timeout++) {
    if (Timeout >= 10000) {
      DEBUG ((DEBUG_ERROR, "Cru: %a did not lock (CON1 0x%08x)\n", mPllNames[Pll], MmioRead32 (Con1)));
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }

  MmioWrite32 (DISP_CRU_MODE, HIWORD (3U << Shift, PLL_MODE_NORMAL << Shift));
  return EFI_SUCCESS;
}

UINT64
EFIAPI
CruSetVopbDclk (
  IN UINT32  Hz
  )
{
  UINT32            Div;
  UINT32            PllHz;
  CRU_PLL_SETTINGS  Settings;
  EFI_STATUS        Status;
  UINT64            Rate;

  Div   = CruVopDclkDivider (Hz);
  PllHz = Hz * Div;
  if ((Div - 1 > 0xFF) || !CruPllFindSettings (PllHz, &Settings)) {
    DEBUG ((DEBUG_ERROR, "Cru: no CPLL setting for dclk %u Hz (CPLL %u Hz)\n", Hz, PllHz));
    return 0;
  }

  DEBUG ((
    DEBUG_INFO,
    "Cru: dclk_vopb %u Hz: CPLL %u Hz (refdiv %u fbdiv %u postdiv %u/%u), divider %u\n",
    Hz,
    PllHz,
    Settings.RefDiv,
    Settings.FbDiv,
    Settings.PostDiv1,
    Settings.PostDiv2,
    Div
    ));

  Status = CruSetPll (CruPllCpll, &Settings);
  if (EFI_ERROR (Status)) {
    return 0;
  }

  // divout (not frac, not 24M), source CPLL
  MmioWrite32 (
    DISP_CRU_CLKSEL (5),
    HIWORD (DCLK_VOPB_SEL_MASK | DCLK_VOPB_PLL_SEL_MASK | DCLK_VOPB_DIV_MASK, Div - 1)
    );

  Rate = CruGetPllRate (CruPllCpll) / Div;
  DEBUG ((
    DEBUG_INFO,
    "Cru: readback CPLL CON0 0x%08x CON1 0x%08x MODE 0x%08x CLKSEL5 0x%08x -> dclk %lu Hz\n",
    MmioRead32 (DISP_CRU_PLL_CON (CruPllCpll, 0)),
    MmioRead32 (DISP_CRU_PLL_CON (CruPllCpll, 1)),
    MmioRead32 (DISP_CRU_MODE),
    MmioRead32 (DISP_CRU_CLKSEL (5)),
    Rate
    ));
  return Rate;
}

VOID
EFIAPI
CruEnableDisplayClocks (
  VOID
  )
{
  UINT32  Con3;
  UINT32  Src;

  // CLKGATE_CON2: aclk_vo_pre 0, dclk_vopb_src 2, _frac 3, dclk_vopb 4,
  // hclk_vo_pre 12, pclk_vo_pre 13
  MmioWrite32 (DISP_CRU_GATE (2), HIWORD (BIT0 | BIT2 | BIT3 | BIT4 | BIT12 | BIT13, 0));
  // CLKGATE_CON3: aclk/hclk/pclk_vo_niu 0-2, aclk_vopb 3, hclk_vopb 4, pclk_mipi_dsi 9
  MmioWrite32 (DISP_CRU_GATE (3), HIWORD (BIT0 | BIT1 | BIT2 | BIT3 | BIT4 | BIT9, 0));
  // CLKGATE_CON16: pclk_mipidsiphy 4
  MmioWrite32 (DISP_CRU_GATE (16), HIWORD (BIT4, 0));
  // The DSI PHY PLL reference: xin24m (PMU CLKSEL_CON2 bit 7), PMU gate 1 bit 10.
  // The PHY PLL maths assume 24 MHz here.
  MmioWrite32 (DISP_PMUCRU_CLKSEL (2), HIWORD (BIT7, 0));
  MmioWrite32 (DISP_PMUCRU_GATE (1), HIWORD (BIT10, 0));

  // aclk_vo_pre is left as it is; say where it runs from for the log.
  Con3 = MmioRead32 (DISP_CRU_CLKSEL (3));
  Src  = (Con3 >> 6) & 3;
  DEBUG ((
    DEBUG_INFO,
    "Cru: display clocks on; aclk_vo %a/%u, gates CON2 0x%04x CON3 0x%04x CON16 0x%04x PMU1 0x%04x\n",
    Src == 0 ? "GPLL" : (Src == 1 ? "CPLL" : "NPLL"),
    (Con3 & 0x1F) + 1,
    MmioRead32 (DISP_CRU_GATE (2)) & 0xFFFF,
    MmioRead32 (DISP_CRU_GATE (3)) & 0xFFFF,
    MmioRead32 (DISP_CRU_GATE (16)) & 0xFFFF,
    MmioRead32 (DISP_PMUCRU_GATE (1)) & 0xFFFF
    ));
  if (Src == 1) {
    DEBUG ((DEBUG_WARN, "Cru: aclk_vo runs from CPLL, which the dclk is about to retune\n"));
  }
}

UINT64
EFIAPI
CruEnablePwmClock (
  IN UINT8  Block
  )
{
  UINT32  Sel;
  UINT32  Shift;
  UINT32  Div;
  UINT64  Parent;

  if (Block > 1) {
    return 0;
  }

  // pclk_pwm0 CLKGATE_CON14 bit 15, pclk_pwm1 CON15 bit 0;
  // clk_pwm0/1 CLKGATE_CON12 bit 5/6.
  if (Block == 0) {
    MmioWrite32 (DISP_CRU_GATE (14), HIWORD (BIT15, 0));
  } else {
    MmioWrite32 (DISP_CRU_GATE (15), HIWORD (BIT0, 0));
  }

  MmioWrite32 (DISP_CRU_GATE (12), HIWORD (Block == 0 ? BIT5 : BIT6, 0));

  // CLKSEL_CON52: clk_pwm0 div 0-6, mux 7; clk_pwm1 div 8-14, mux 15
  // (0 = GPLL, 1 = xin24m). U-Boot assumes GPLL; read the mux instead.
  Sel    = MmioRead32 (DISP_CRU_CLKSEL (52));
  Shift  = (Block == 0) ? 0 : 8;
  Div    = (Sel >> Shift) & 0x7F;
  Parent = ((Sel >> (Shift + 7)) & 1) ? CRU_PLL_OSC_HZ : CruGetPllRate (CruPllGpll);
  return Parent / (Div + 1);
}

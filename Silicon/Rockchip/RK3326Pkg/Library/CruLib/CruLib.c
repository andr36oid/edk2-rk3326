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


UINTN
CruGetSdmmcClockRate (
    IN UINT8 Index
    )
{
    UINT32 Val;
    UINT32 Sel;
    UINT32 Shift, Mask;
    UINTN Rate;
    
    ASSERT (Index <= 1);

    Shift = CRU_CLKSEL_CON17_CLK_SDMMC_SEL_SHIFT;
    Mask = CRU_CLKSEL_CON17_CLK_SDMMC_SEL_MASK;


    Val = MmioRead32 (CRU_CLKSEL_CON (30));
    DEBUG ((DEBUG_INFO, "CruGetSdmmcClockRate(): CRU_CLKSEL_CON30 = %08X\n", Val));
    Sel = (Val & Mask) >> Shift;

    switch (Sel) {
    case 0:
        Rate = 24000000U;
        break;
    case 1:
        Rate = 400000000U;
        break;
    case 2:
        Rate = 300000000U;
        break;
    case 3:
        Rate = 100000000U;
        break;
    case 4:
        Rate = 50000000U;
        break;
    case 5:
        Rate = 750000U;
        break;
    default:
        ASSERT (FALSE);
        Rate = 0;
    }

    return Rate;
}

VOID
CruSetSdmmcClockRate (
  IN UINT8 Index,
  IN UINTN Rate
  )
{
    UINT32 Val;
    UINT32 Sel;
    UINT32 Shift, Mask;
    EFI_PHYSICAL_ADDRESS Reg;
    
    ASSERT (Index <= 2);

    if (Index == 0) {
        Reg = CRU_CLKSEL_CON (16);
        Shift = CRU_CLKSEL_CON16_CLK_SDMMC_SEL_SHIFT;
        Mask = CRU_CLKSEL_CON16_CLK_SDMMC_SEL_MASK;
    } else if (Index == 1) {
        Reg = CRU_CLKSEL_CON (17);
        Shift = CRU_CLKSEL_CON17_CLK_SDMMC_SEL_SHIFT;
        Mask = CRU_CLKSEL_CON17_CLK_SDMMC_SEL_MASK;
    }


    //
    // Select PLL or mux value based on requested rate
    // PX30 mapping: SDMMC0 pll_sel
    //   0 = GPLL, 1 = CPLL, 2 = NPLL, 3 = xin_osc0
    //
    if (Index == 0) {
        if (Rate <= 750000U) {
            Sel = 3; // xin_osc0
        } else if (Rate <= 24000000U) {
            Sel = 0; // GPLL
        } else if (Rate <= 50000000U) {
            Sel = 2; // NPLL
        } else if (Rate <= 100000000U) {
            Sel = 1; // CPLL
        } else {
            ASSERT(FALSE);
        }
    } else {
        // SDMMC1 mux: 0 = clk_sdmmc, 1 = clk_sdmmc_div50
        Sel = (Rate > 100000000U) ? 1 : 0;
    }

    Val = Mask << 16;
    Val |= Sel << Shift;
    MmioWrite32 (Reg, Val);

    DEBUG ((DEBUG_INFO, "CruSetSdmmcClockRate(%u, %lu): 0x%08X = %08X (wrote %08X)\n", Index, Rate, (UINT32)Reg, MmioRead32 (Reg), Val));
}

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

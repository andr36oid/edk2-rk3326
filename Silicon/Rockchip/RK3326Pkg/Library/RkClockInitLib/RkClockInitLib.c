/** @file
  PLL and bus clock setup, after U-Boot drivers/clk/rockchip/clk_px30.c
  (px30_clk_probe, px30_armclk_set_clk, px30_gpll_set_pmuclk, rkclk_set_pll).

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/CruLib.h>
#include <Library/RkClockInitLib.h>

#include "RkClockMath.h"

#define CRU_APLL_CON0     (CRU_BASE + 0x00)
#define CRU_MODE          (CRU_BASE + 0xA0)
#define PMUCRU_GPLL_CON0  (PMUCRU_BASE + 0x00)
#define PMUCRU_MODE_REG   (PMUCRU_BASE + 0x20)

// GRF-style write: the upper half says which bits of the lower half to take.
#define HIWORD(Mask, Val)  (((UINT32)(Mask) << 16) | ((UINT32)(Val) & (Mask)))

// U-Boot waits forever for the lock; a PLL normally locks in well under 1 ms.
#define PLL_LOCK_TRIES  10000

STATIC
UINT32
PllHz (
  IN UINTN  Con0Addr,
  IN UINTN  ModeAddr
  )
{
  if ((MmioRead32 (ModeAddr) & RKCLK_MODE_MASK) != RKCLK_MODE_PLL) {
    return RKCLK_OSC_HZ;
  }

  return RkClkPllHz (MmioRead32 (Con0Addr), MmioRead32 (Con0Addr + 4));
}

//
// rkclk_set_pll: crystal while it changes, integer mode, power down, divisors,
// power up, wait for lock, back to the PLL.
//
STATIC
EFI_STATUS
PllSet (
  IN UINTN                     Con0Addr,
  IN UINTN                     ModeAddr,
  IN CONST RKCLK_PLL_PARAMS    *P
  )
{
  UINTN  Tries;

  MmioWrite32 (ModeAddr, HIWORD (RKCLK_MODE_MASK, RKCLK_MODE_SLOW));
  MmioWrite32 (Con0Addr + 4, HIWORD (RKCLK_PLL_DSMPD, RKCLK_PLL_DSMPD));
  MmioWrite32 (Con0Addr + 4, HIWORD (RKCLK_PLL_PD, RKCLK_PLL_PD));
  MmioWrite32 (
    Con0Addr,
    HIWORD (RKCLK_PLL_POSTDIV1_MASK | RKCLK_PLL_FBDIV_MASK, (P->PostDiv1 << RKCLK_PLL_POSTDIV1_SHIFT) | P->FbDiv)
    );
  MmioWrite32 (
    Con0Addr + 4,
    HIWORD (RKCLK_PLL_POSTDIV2_MASK | RKCLK_PLL_REFDIV_MASK, (P->PostDiv2 << RKCLK_PLL_POSTDIV2_SHIFT) | P->RefDiv)
    );
  MmioWrite32 (Con0Addr + 4, HIWORD (RKCLK_PLL_PD, 0));

  for (Tries = 0; (MmioRead32 (Con0Addr + 4) & RKCLK_PLL_LOCK) == 0; Tries++) {
    if (Tries >= PLL_LOCK_TRIES) {
      DEBUG ((DEBUG_ERROR, "RkClock: PLL at 0x%lX did not lock (con0 0x%X con1 0x%X)\n", Con0Addr, MmioRead32 (Con0Addr), MmioRead32 (Con0Addr + 4)));
      return EFI_DEVICE_ERROR;
    }

    MicroSecondDelay (1);
  }

  MmioWrite32 (ModeAddr, HIWORD (RKCLK_MODE_MASK, RKCLK_MODE_PLL));
  return EFI_SUCCESS;
}

//
// The bus dividers for a given GPLL. Written as whole registers, only when the
// readback differs.
//
STATIC
VOID
BusDividersSet (
  IN UINT32  Gpll
  )
{
  RKCLK_BUS_DIVS  D;
  UINT32          Cur;

  RkClkBusDivs (Gpll, &D);

  Cur = MmioRead32 (CRU_CLKSEL_CON (23)) & RKCLK_CON23_MASK;
  if (Cur != D.Con23) {
    MmioWrite32 (CRU_CLKSEL_CON (23), HIWORD (RKCLK_CON23_MASK, D.Con23));
  }

  Cur = MmioRead32 (CRU_CLKSEL_CON (24)) & RKCLK_CON24_MASK;
  if (Cur != D.Con24) {
    MmioWrite32 (CRU_CLKSEL_CON (24), HIWORD (RKCLK_CON24_MASK, D.Con24));
  }

  Cur = MmioRead32 (CRU_CLKSEL_CON (14)) & RKCLK_CON14_MASK;
  if (Cur != D.Con14) {
    MmioWrite32 (CRU_CLKSEL_CON (14), HIWORD (RKCLK_CON14_MASK, D.Con14));
  }

  DEBUG ((
    DEBUG_INFO,
    "RkClock: gpll %u Hz: CLKSEL23 0x%X 24 0x%X 14 0x%X\n",
    Gpll,
    MmioRead32 (CRU_CLKSEL_CON (23)),
    MmioRead32 (CRU_CLKSEL_CON (24)),
    MmioRead32 (CRU_CLKSEL_CON (14))
    ));
}

//
// px30_armclk_set_clk. The core source is APLL, divider 1:1. When the rate goes
// down the PLL moves first, when it goes up the core dividers do. U-Boot does
// not move ARMCLK off APLL either: the PLL sits on the 24 MHz crystal while it
// relocks, so the core never sees an unlocked clock.
//
STATIC
EFI_STATUS
ArmClockSet (
  IN BOOLEAN  Force
  )
{
  CONST RKCLK_PLL_PARAMS  *P;
  CONST RKCLK_CPU_PARAMS  *Cpu;
  EFI_STATUS              Status;
  UINT32                  Old;
  UINT32                  Want;

  P   = RkClkPllParams (RKCLK_APLL_HZ);
  Cpu = &mRkClkCpuRates[0];
  ASSERT (P != NULL && Cpu->Rate == RKCLK_APLL_HZ);

  Old    = PllHz (CRU_APLL_CON0, CRU_MODE);
  Want   = RKCLK_CORE_VALUE (Cpu->AclkDiv, Cpu->PclkDiv);
  Status = EFI_SUCCESS;
  DEBUG ((DEBUG_INFO, "RkClock: apll %u Hz, CLKSEL0 0x%X\n", Old, MmioRead32 (CRU_CLKSEL_CON (0))));

  if (!Force && (Old == RKCLK_APLL_HZ)) {
    // Already there; only fix the core dividers if they differ.
    if ((MmioRead32 (CRU_CLKSEL_CON (0)) & RKCLK_CORE_MASK) != Want) {
      MmioWrite32 (CRU_CLKSEL_CON (0), HIWORD (RKCLK_CORE_MASK, Want));
    }
  } else if (Old > RKCLK_APLL_HZ) {
    Status = PllSet (CRU_APLL_CON0, CRU_MODE, P);
    MmioWrite32 (CRU_CLKSEL_CON (0), HIWORD (RKCLK_CORE_MASK, Want));
  } else {
    MmioWrite32 (CRU_CLKSEL_CON (0), HIWORD (RKCLK_CORE_MASK, Want));
    Status = PllSet (CRU_APLL_CON0, CRU_MODE, P);
  }

  DEBUG ((
    DEBUG_INFO,
    "RkClock: apll now %u Hz, CLKSEL0 0x%X (%r)\n",
    PllHz (CRU_APLL_CON0, CRU_MODE),
    MmioRead32 (CRU_CLKSEL_CON (0)),
    Status
    ));
  return Status;
}

EFI_STATUS
EFIAPI
RkClockInit (
  IN BOOLEAN  Force
  )
{
  UINT32      Gpll;
  UINT32      Hi;
  EFI_STATUS  Status;
  EFI_STATUS  Result;

  Result = EFI_SUCCESS;
  Gpll   = PllHz (PMUCRU_GPLL_CON0, PMUCRU_MODE_REG);
  DEBUG ((DEBUG_INFO, "RkClock: gpll %u Hz (force %d)\n", Gpll, Force));

  if (Force || (Gpll != RKCLK_GPLL_HZ)) {
    //
    // The buses hang off GPLL. Size their dividers for the higher of the old
    // and new rate first, so no bus runs over its limit while GPLL changes.
    //
    Hi = (Gpll > RKCLK_GPLL_HZ) ? Gpll : RKCLK_GPLL_HZ;
    BusDividersSet (Hi);
    Status = PllSet (PMUCRU_GPLL_CON0, PMUCRU_MODE_REG, RkClkPllParams (RKCLK_GPLL_HZ));
    if (EFI_ERROR (Status)) {
      Result = Status;
    }

    Gpll = PllHz (PMUCRU_GPLL_CON0, PMUCRU_MODE_REG);
    DEBUG ((DEBUG_INFO, "RkClock: gpll now %u Hz (%r)\n", Gpll, Status));
  }

  BusDividersSet (Gpll);

  Status = ArmClockSet (Force);
  if (EFI_ERROR (Status)) {
    Result = Status;
  }

  return Result;
}

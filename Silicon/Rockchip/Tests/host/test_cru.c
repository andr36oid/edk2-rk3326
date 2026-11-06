// CruLib's SD/eMMC clock code against a model of the PX30 CRU: hiword-masked
// CLKSEL/CLKGATE/SOFTRST writes, PMUCRU GPLL registers.
#include <Uefi.h>
#include <Library/CruLib.h>
#include "test.h"

#define NREG 0x500
static UINT32 cru[NREG / 4], pmucru[0x100 / 4];

static UINT32 *reg (UINTN a)
{
  if (a >= CRU_BASE && a < CRU_BASE + NREG) return &cru[(a - CRU_BASE) / 4];
  if (a >= PMUCRU_BASE && a < PMUCRU_BASE + 0x100) return &pmucru[(a - PMUCRU_BASE) / 4];
  printf ("FAIL: access outside the CRU: 0x%lx\n", (unsigned long)a);
  exit (1);
}

UINT32 EFIAPI MmioRead32 (UINTN a) { return *reg (a); }

UINT32 EFIAPI MmioWrite32 (UINTN a, UINT32 v)
{
  UINT32 *r = reg (a);
  if (a >= CRU_BASE && a < CRU_BASE + NREG && (a - CRU_BASE) >= 0x100) {
    UINT32 m = v >> 16;                       // hiword write-enable
    *r = (*r & ~m) | (v & m & 0xFFFF);
  } else {
    *r = v;
  }
  return v;
}

static void gpll (UINT32 fb, UINT32 ref, UINT32 p1, UINT32 p2, UINT32 mode)
{
  pmucru[0] = (p1 << 12) | fb;
  pmucru[1] = (p2 << 6) | ref;
  pmucru[0x20 / 4] = mode;
}

#define CLKSEL(n)  cru[(0x100 + (n) * 4) / 4]
#define GATE(n)    cru[(0x200 + (n) * 4) / 4]

int main (void)
{
  // U-Boot's GPLL: 24 MHz * 100 / (1 * 2 * 1) = 1200 MHz
  gpll (100, 1, 2, 1, 1);
  CHECK_EQ (CruGetGpllRate (), 1200000000);

  // SD at 50 MHz cclk_in: CRU 100 MHz = GPLL / 12
  CLKSEL (16) = 0xC0FF; CLKSEL (17) = 0x8000;      // junk: 24M source, div50 path
  GATE (6) = 0xFFFF; GATE (7) = 0xFFFF;             // everything gated
  CruSetSdmmcClockRate (0, 50000000);
  CHECK_EQ (CLKSEL (16) & 0xC0FF, (0 << 14) | 11);
  CHECK_EQ (CLKSEL (17) & 0x8000, 0);
  CHECK_EQ (GATE (6) & 0xF000, 0);                  // bits 12..15 open
  CHECK_EQ (GATE (7) & 0x0003, 0);                  // hclk_sdmmc(+niu) open
  CHECK_EQ (GATE (6) & 0x0FFF, 0x0FFF);             // eMMC and others untouched
  CHECK_EQ (CruGetSdmmcClockRate (0), 50000000);

  // eMMC the same way, in CLKSEL 20/21 and its own gates
  CruSetSdmmcClockRate (1, 50000000);
  CHECK_EQ (CLKSEL (20) & 0xC0FF, 11);
  CHECK_EQ (GATE (6) & ((1 << 0) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 10)), 0);
  CHECK_EQ (CruGetSdmmcClockRate (1), 50000000);

  // Never above the asked rate: 52 MHz from 1200 MHz -> /12 = 50 MHz
  CruSetSdmmcClockRate (0, 52000000);
  CHECK (CruGetSdmmcClockRate (0) <= 52000000);
  CHECK_EQ (CruGetSdmmcClockRate (0), 50000000);

  // 400 kHz needs more than /128 from GPLL: 24 MHz source, /30
  CruSetSdmmcClockRate (0, 400000);
  CHECK_EQ ((CLKSEL (16) >> 14) & 3, 3);
  CHECK_EQ (CruGetSdmmcClockRate (0), 400000);

  // GPLL bypassed (24 MHz): 50 MHz asked -> 24/1/2 = 12 MHz, still not above
  gpll (100, 1, 2, 1, 0);
  CHECK_EQ (CruGetGpllRate (), 24000000);
  CruSetSdmmcClockRate (0, 50000000);
  CHECK_EQ (CruGetSdmmcClockRate (0), 12000000);

  // Unreadable GPLL (zero dividers): falls back to the crystal
  gpll (100, 0, 2, 1, 1);
  CHECK_EQ (CruGetGpllRate (), 0);
  CruSetSdmmcClockRate (0, 50000000);
  CHECK_EQ ((CLKSEL (16) >> 14) & 3, 3);

  // Bad index: no writes
  UINT32 before = CLKSEL (16);
  CruSetSdmmcClockRate (2, 50000000);
  CHECK_EQ (CLKSEL (16), before);
  CHECK_EQ (CruGetSdmmcClockRate (2), 0);
  DONE ();
}

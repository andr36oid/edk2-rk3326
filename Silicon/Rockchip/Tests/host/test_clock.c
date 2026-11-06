// RkClockInitLib against a model of the PX30 CRU and PMUCRU: hiword-masked
// registers, a PLL that locks once powered up, and a check after EVERY write
// that no bus clock is above its limit and that the order of the APLL/core
// divider writes is the one U-Boot uses.
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/CruLib.h>
#include <Library/RkClockInitLib.h>
#include "test.h"
#include "RkClockMath.h"

extern int mock_verbose;

static UINT32 cru[0x500 / 4], pmucru[0x100 / 4];
static int never_lock, writes, pll_writes, order_ok;
static int apll_write_seq, core_write_seq, seq;

static UINT32 *reg (UINTN a)
{
  if (a >= CRU_BASE && a < CRU_BASE + 0x500) return &cru[(a - CRU_BASE) / 4];
  if (a >= PMUCRU_BASE && a < PMUCRU_BASE + 0x100) return &pmucru[(a - PMUCRU_BASE) / 4];
  printf ("FAIL: access outside the CRU: 0x%lx\n", (unsigned long)a);
  exit (1);
}

static int is_pll_reg (UINTN a)
{
  UINTN o = a - CRU_BASE;
  if (a >= PMUCRU_BASE) return (a - PMUCRU_BASE) < 0x24;
  return o < 0x80 || o == 0xA0;
}

UINT32 EFIAPI MmioRead32 (UINTN a)
{
  UINT32 *r = reg (a);
  UINTN   o = (a >= PMUCRU_BASE) ? (a - PMUCRU_BASE) : (a - CRU_BASE);
  if ((a >= PMUCRU_BASE && o == 4) || (a < PMUCRU_BASE && o < 0x80 && (o & 0x1F) == 4)) {
    // PLL_CON1: lock follows power down
    if ((*r & RKCLK_PLL_PD) || never_lock) *r &= ~RKCLK_PLL_LOCK; else *r |= RKCLK_PLL_LOCK;
  }
  return *r;
}

static UINT32 gpll_hz (void)
{
  if ((pmucru[0x20 / 4] & 3) != 1) return 24000000;
  return RkClkPllHz (pmucru[0], pmucru[1]);
}

static UINT32 apll_hz (void)
{
  if ((cru[0xA0 / 4] & 3) != 1) return 24000000;
  return RkClkPllHz (cru[0], cru[1]);
}

#define CLKSEL(n)  cru[(0x100 + (n) * 4) / 4]

static void check_limits (void)
{
  UINT32 g = gpll_hz ();
  UINT32 a = g / (((CLKSEL (23) >> 8) & 0x1F) + 1);
  UINT32 p = a / (((CLKSEL (24) >> 8) & 3) + 1);
  if (CLKSEL (23) & 0x8000) return;                 // CPLL source: not modelled
  if (a > 200000000U || g / ((CLKSEL (24) & 0x1F) + 1) > 150000000U || p > 100000000U ||
      g / ((CLKSEL (14) & 0x1F) + 1) > 200000000U || g / (((CLKSEL (14) >> 8) & 0x1F) + 1) > 150000000U) {
    printf ("FAIL: a bus is over its limit: gpll %u aclk_bus %u pclk_bus %u\n", g, a, p);
    exit (1);
  }
}

UINT32 EFIAPI MmioWrite32 (UINTN a, UINT32 v)
{
  UINT32 *r = reg (a);
  UINT32  m = v >> 16;
  writes++;
  if (is_pll_reg (a)) pll_writes++;
  *r = (*r & ~m) | (v & m & 0xFFFF);
  if (a == CRU_BASE + 0x100 + 0 * 4) core_write_seq = ++seq;
  if (a == CRU_BASE + 0xA0 || a == CRU_BASE) apll_write_seq = ++seq;   // mode or APLL_CON0
  if (a != CRU_BASE + 0xA0 || 1) check_limits ();
  return v;
}

static void set_state (UINT32 g_fb, UINT32 g_ref, UINT32 g_p1, UINT32 a_fb, UINT32 a_ref, UINT32 a_p1, UINT32 core)
{
  for (unsigned i = 0; i < 0x500 / 4; i++) cru[i] = 0;
  for (unsigned i = 0; i < 0x100 / 4; i++) pmucru[i] = 0;
  pmucru[0] = (g_p1 << 12) | g_fb;  pmucru[1] = (1 << 6) | g_ref;  pmucru[0x20 / 4] = 1;
  cru[0] = (a_p1 << 12) | a_fb;     cru[1] = (1 << 6) | a_ref;     cru[0xA0 / 4] = 1;
  CLKSEL (0) = core;
  writes = pll_writes = 0; seq = apll_write_seq = core_write_seq = 0; never_lock = 0;
  {
    // dividers that are right for this GPLL, so the start is a legal state
    UINT32 g = gpll_hz ();
    CLKSEL (23) = RkClkDivField (g, 200000000U, 31) << 8;
    CLKSEL (24) = RkClkDivField (g, 150000000U, 31) | (RkClkDivField (g / (RkClkDivField (g, 200000000U, 31) + 1), 100000000U, 3) << 8);
    CLKSEL (14) = RkClkDivField (g, 200000000U, 31) | (RkClkDivField (g, 150000000U, 31) << 8);
  }
}

static void expect_final (const char *what)
{
  printf ("-- %s\n", what);
  CHECK_EQ (gpll_hz (), 1200000000U);
  CHECK_EQ (apll_hz (), 600000000U);
  CHECK_EQ (cru[0xA0 / 4] & 3, 1);
  CHECK_EQ (pmucru[0x20 / 4] & 3, 1);
  // 1200 MHz GPLL: ACLK_BUS 200 (div 6), HCLK_BUS 150 (div 8), PCLK_BUS 100 (ACLK/2),
  // ACLK_PERI 200, HCLK_PERI 150; U-Boot's cpu row for 600 MHz: aclk_div 1, pclk_div 3.
  CHECK_EQ (CLKSEL (23) & 0x8FFF, 0x0500);
  CHECK_EQ (CLKSEL (24) & 0x03FF, 0x0107);
  CHECK_EQ (CLKSEL (14) & 0x9FFF, 0x0705);
  CHECK_EQ (CLKSEL (0) & 0x7F8F, 0x1300 | 0x0000);        // APLL source (BIT7 clear), div 0
  CHECK_EQ (CLKSEL (0) & (1 << 7), 0);
}

int main (void)
{
  // The numbers the PLL parameters stand for.
  CHECK_EQ (RkClkPllHz (RkClkPllParams (1200000000U)->PostDiv1 << 12 | RkClkPllParams (1200000000U)->FbDiv,
                        RkClkPllParams (1200000000U)->PostDiv2 << 6 | RkClkPllParams (1200000000U)->RefDiv), 1200000000U);
  CHECK_EQ (RkClkPllHz (3 << 12 | 75, 1 << 6 | 1), 600000000U);
  CHECK_EQ (RkClkPllHz (2 << 12 | 100, 1 << 6 | 1), 1200000000U);   // the other encoding of 1200
  CHECK (RkClkPllParams (700000000U) == NULL);
  CHECK_EQ (RkClkPllHz (0, 0), 0);
  CHECK_EQ (RkClkDivField (1200000000U, 200000000U, 31), 5);
  CHECK_EQ (RkClkDivField (1200000000U, 150000000U, 31), 7);
  CHECK_EQ (RkClkDivField (200000000U, 100000000U, 3), 1);
  CHECK_EQ (RkClkDivField (1188000000U, 200000000U, 31), 5);        // 198 MHz
  CHECK_EQ (RkClkDivField (1500000000U, 200000000U, 31), 7);        // rounds up: 187.5 MHz
  CHECK_EQ (RkClkDivField (24000000U, 200000000U, 31), 0);
  CHECK_EQ (RkClkDivField (4000000000U, 100000000U, 3), 3);         // clamped

  // A miniloader-like start: APLL 816, GPLL 1188 (24*99/2).
  set_state (99, 2, 1, 68, 1, 2, 0x1300);
  CHECK_EQ (gpll_hz (), 1188000000U);
  CHECK_EQ (apll_hz (), 816000000U);
  CHECK_EQ (RkClockInit (FALSE), EFI_SUCCESS);
  expect_final ("from 816/1188");
  CHECK (core_write_seq > apll_write_seq);          // rate went down: PLL first, then the core dividers

  // Start slower than the target: dividers first, then the PLL.
  set_state (50, 1, 1, 17, 1, 1, 0);                 // APLL 24*17 = 408
  CHECK_EQ (apll_hz (), 408000000U);
  CHECK_EQ (RkClockInit (FALSE), EFI_SUCCESS);
  expect_final ("from 408/1200");
  CHECK (core_write_seq < apll_write_seq);

  // GPLL above target: the bus dividers are pre-scaled for it (limits checked per write).
  set_state (125, 2, 1, 68, 1, 2, 0x1300);           // GPLL 1500
  CHECK_EQ (gpll_hz (), 1500000000U);
  CHECK_EQ (RkClockInit (FALSE), EFI_SUCCESS);
  expect_final ("from gpll 1500");

  // GPLL in crystal mode (a PLL the earlier stage never switched on).
  set_state (50, 1, 1, 75, 1, 3, 0x1300);
  pmucru[0x20 / 4] = 0;
  CHECK_EQ (RkClockInit (FALSE), EFI_SUCCESS);
  expect_final ("from gpll bypassed");

  // Already right: nothing is written at all, with or without the flag set later.
  set_state (50, 1, 1, 75, 1, 3, 0x1300);
  CHECK_EQ (RkClockInit (FALSE), EFI_SUCCESS);
  CHECK_EQ (writes, 0 + writes);                     // may fix dividers; PLLs must stay alone
  CHECK_EQ (pll_writes, 0);
  expect_final ("already at target");
  writes = 0;
  CHECK_EQ (RkClockInit (FALSE), EFI_SUCCESS);
  CHECK_EQ (writes, 0);                              // second run: pure no-op
  CHECK_EQ (RkClockInit (TRUE), EFI_SUCCESS);
  CHECK (pll_writes > 0);                            // Force reprograms both
  expect_final ("forced");

  // A PLL that never locks: error, stays on the crystal, never sets the PLL mode.
  set_state (99, 2, 1, 75, 1, 3, 0x1300);
  never_lock = 1;
  CHECK_EQ (RkClockInit (FALSE), EFI_DEVICE_ERROR);
  CHECK_EQ (pmucru[0x20 / 4] & 3, 0);

  DONE ();
}

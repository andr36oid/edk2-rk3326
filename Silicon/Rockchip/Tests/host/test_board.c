// BoardInit.c (pins, GPIOs, IO domains, eMMC pins) against a model of the
// SYS_GRF / PMU_GRF (hiword write masks) and the four GPIO banks. The clock
// library is stubbed (test_clock covers it). Expected pins come from the
// inventory (section 5) and U-Boot's odroidgoa.c / board.c, not from the code
// under test; the eMMC values are U-Boot's literals.
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/CruLib.h>
#include <Library/GpioLib.h>
#include <Library/RkClockInitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "BoardInit.h"
#include "GpioRegs.h"

static UINT32 sysgrf[0x200 / 4], pmugrf[0x200 / 4], gpio[4][0x100 / 4];
static int clock_calls, clock_force, gpio_clk_calls;

EFI_STATUS EFIAPI RkClockInit (BOOLEAN Force) { clock_calls++; clock_force = Force; return EFI_SUCCESS; }
VOID EFIAPI CruEnableGpioClocks (VOID) { gpio_clk_calls++; }

static UINT32 *reg (UINTN a, int *hiword)
{
  *hiword = 1;
  if (a >= SYS_GRF && a < SYS_GRF + 0x200) return &sysgrf[(a - SYS_GRF) / 4];
  if (a >= PMU_GRF && a < PMU_GRF + 0x200) return &pmugrf[(a - PMU_GRF) / 4];
  *hiword = 0;
  if (a >= GPIO_BANK0 && a < GPIO_BANK0 + 0x100) return &gpio[0][(a - GPIO_BANK0) / 4];
  for (int g = 1; g < 4; g++) {
    UINTN b = GPIO_BASE (g);
    if (a >= b && a < b + 0x100) return &gpio[g][(a - b) / 4];
  }
  printf ("FAIL: unexpected access 0x%lx\n", (unsigned long)a);
  exit (1);
}

UINT32 EFIAPI MmioRead32 (UINTN a) { int h; return *reg (a, &h); }
UINT32 EFIAPI MmioWrite32 (UINTN a, UINT32 v)
{
  int h; UINT32 *r = reg (a, &h);
  if (h) { UINT32 m = v >> 16; *r = (*r & ~m) | (v & m & 0xFFFF); } else *r = v;
  return v;
}

static void fresh (void)
{
  for (unsigned i = 0; i < 0x200 / 4; i++) { sysgrf[i] = 0xFFFF; pmugrf[i] = 0xFFFF; }   // worst case: everything set
  memset (gpio, 0, sizeof (gpio));
  clock_calls = gpio_clk_calls = 0;
}

static UINT32 field (UINT32 *base, UINTN grf, GPIO_REG_FIELD f)
{
  return (base[(f.Reg - grf) / 4] >> f.Shift) & f.Mask;
}

static UINT32 pull (UINT8 g, UINT8 p) { GPIO_REG_FIELD f; GpioPullField (g, p, &f); return field (sysgrf, SYS_GRF, f); }
static UINT32 mux (UINT8 g, UINT8 p) { GPIO_REG_FIELD f; GpioIomuxField (g, p, &f); return field (g ? sysgrf : pmugrf, g ? SYS_GRF : PMU_GRF, f); }
static int out (int g, int p) { return (gpio[g][GPIO_SWPORT_DDR / 4] >> p) & 1; }
static int lvl (int g, int p) { return (gpio[g][GPIO_SWPORT_DR / 4] >> p) & 1; }

static void *slurp (const char *path)
{
  FILE *f = fopen (path, "rb");
  if (!f) return NULL;
  void *b = malloc (1 << 20);
  size_t n = fread (b, 1, 1 << 20, f);
  fclose (f);
  return n ? b : NULL;
}

int main (int argc, char **argv)
{
  BOARD_INIT_PLAN plan;
  const char *base = "/andr36oid/src/device/gameconsole/r36s/BOOT/Devices/";

  // --- SD boot, no U-Boot: pins on, eMMC off, no FDT
  fresh ();
  plan = (BOARD_INIT_PLAN){ .Force = FALSE, .Pins = TRUE, .EmmcPins = FALSE, .Fdt = NULL };
  CHECK_EQ (BoardInitRun (&plan), EFI_SUCCESS);
  CHECK_EQ (clock_calls, 1);
  CHECK_EQ (clock_force, 0);
  CHECK_EQ (gpio_clk_calls, 1);
  // Button pins: function 0 (GPIO) on GPIO1_A2, A5-A7, B4-B7; others untouched (still 0xF)
  int gpio_mux[] = { 2, 5, 6, 7, 12, 13, 14, 15 };
  for (int p = 0; p < 16; p++) {
    int is_btn = 0;
    for (unsigned i = 0; i < 8; i++) is_btn |= (gpio_mux[i] == p);
    CHECK_EQ (mux (1, p), is_btn ? 0 : 0xF);
  }
  // Pull-ups (code 1): GPIO1_B4-B7, A3, A5-A7, GPIO2_A0-A7, GPIO3_B2, B7; everything else keeps its 3
  for (int g = 1; g <= 3; g++) {
    for (int p = 0; p < 32; p++) {
      int up = 0;
      if (g == 1 && ((p >= 12 && p <= 15) || p == 3 || (p >= 5 && p <= 7))) up = 1;
      if (g == 2 && p < 8) up = 1;
      if (g == 3 && (p == 10 || p == 15)) up = 1;
      if (g == 1 && p >= 16) continue;      // GPIO1_C/D not modelled in the pull regs we touch
      CHECK_EQ (pull (g, p), up ? 1 : 3);
    }
  }
  // UART2 on M1: iofunc_con0 bits 11:10 == 1, the other bits untouched
  CHECK_EQ ((sysgrf[GRF_IOFUNC_CON0 / 4] >> 10) & 3, 1);
  CHECK_EQ (sysgrf[GRF_IOFUNC_CON0 / 4] & ~(3u << 10), 0xFFFF & ~(3u << 10));
  // IO domains: vccio6 source switch set, all 3.3 V (0); pmuio1/2 3.3 V. oscgpi (bit 7) untouched.
  CHECK_EQ (sysgrf[GRF_IO_VSEL / 4], 0xFF81);
  CHECK_EQ (pmugrf[PMUGRF_IO_VSEL / 4], 0x3FFF);
  // GPIOs
  CHECK_EQ (out (0, 15), 1); CHECK_EQ (lvl (0, 15), 0);    // rumble low
  CHECK_EQ (out (0, 18), 1); CHECK_EQ (lvl (0, 18), 1);    // power LED high
  CHECK_EQ (out (3, 14), 1); CHECK_EQ (lvl (3, 14), 1);    // Wi-Fi enable high
  CHECK_EQ (out (0, 13), 0);                                // no FDT: charge LED pin left alone
  // eMMC pins untouched
  CHECK_EQ (mux (1, 0), 0xF);

  // --- eMMC boot: U-Boot's literals land, function 2 on GPIO1_A0-A7 and B1-B2
  fresh ();
  plan.EmmcPins = TRUE;
  CHECK_EQ (BoardInitRun (&plan), EFI_SUCCESS);
  for (int p = 0; p < 8; p++) { CHECK_EQ (mux (1, p), 2); CHECK_EQ (pull (1, p), 1); }
  CHECK_EQ (mux (1, 9), 2); CHECK_EQ (mux (1, 10), 2);
  CHECK_EQ (pull (1, 9), 0);                // clock: no pull
  CHECK_EQ (pull (1, 10), 1);               // command: pull-up
  CHECK_EQ (sysgrf[GRF_GPIO1A_E / 4], 0xAAAA);          // 8 mA on A0-A7
  CHECK_EQ (sysgrf[GRF_GPIO1B_E / 4] & 0x003C, 0x0028); // B1 and B2
  CHECK_EQ (mux (1, 8), 0xF);               // GPIO1_B0 left alone
  // The eMMC writes come after the button pins, so A2, A5-A7 end as eMMC data.
  CHECK_EQ (mux (1, 2), 2);

  // --- U-Boot ran before: only clocks and IO domains (the caller sets Pins FALSE)
  fresh ();
  plan = (BOARD_INIT_PLAN){ .Force = FALSE, .Pins = FALSE, .EmmcPins = FALSE, .Fdt = NULL };
  CHECK_EQ (BoardInitRun (&plan), EFI_SUCCESS);
  CHECK_EQ (clock_calls, 1);
  CHECK_EQ (sysgrf[GRF_IO_VSEL / 4], 0xFF81);
  CHECK_EQ (gpio_clk_calls, 0);
  CHECK_EQ (out (0, 15) + out (0, 18) + out (3, 14), 0);
  CHECK_EQ (mux (1, 2), 0xF);
  CHECK_EQ (sysgrf[GRF_IOFUNC_CON0 / 4], 0xFFFF);
  plan.Force = TRUE;
  BoardInitRun (&plan);
  CHECK_EQ (clock_force, 1);

  // --- Charge LED against real screen device trees
  struct { const char *dtb; int led; } dts[] = {
    { "D-28S/rg351mp-uboot.dtb", 0 },     // enable-gpios on GPIO0_B5: leave alone
    { "G350/rg351mp-uboot.dtb",  1 },     // enable-gpios on GPIO0_C2, not B5
    { "R45H/rg351mp-uboot.dtb",  1 },
    { "R50S/rg351p-uboot.dtb",   1 },
  };
  for (unsigned i = 0; i < sizeof (dts) / sizeof (dts[0]); i++) {
    char path[512];
    snprintf (path, sizeof (path), "%s%s", base, dts[i].dtb);
    void *fdt = slurp (path);
    if (!fdt) { printf ("(no %s, skipped)\n", path); continue; }
    fresh ();
    plan = (BOARD_INIT_PLAN){ .Force = FALSE, .Pins = TRUE, .EmmcPins = FALSE, .Fdt = fdt };
    BoardInitRun (&plan);
    CHECK_EQ (out (0, 13), dts[i].led);
    CHECK_EQ (lvl (0, 13), 0);
    CHECK_EQ (out (0, 15), 1);               // the other pins do not depend on the FDT
    free (fdt);
  }
  DONE ();
}

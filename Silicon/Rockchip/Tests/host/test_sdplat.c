// RkSdmmcPlatformLib (SD card pins and card detect) with the real GpioLib on
// a model of the PMU/SYS GRF (hiword write masks) and the GPIO banks. The
// expected pins are the kernel's and U-Boot's px30.dtsi: the sdmmc node takes
// pinctrl-0 = <&sdmmc_clk &sdmmc_cmd &sdmmc_det &sdmmc_bus4>, with
//   sdmmc_clk  <1 RK_PD6 RK_FUNC_1 &pcfg_pull_none_8ma>
//   sdmmc_cmd  <1 RK_PD7 RK_FUNC_1 &pcfg_pull_up_8ma>
//   sdmmc_det  <0 RK_PA3 RK_FUNC_1 &pcfg_pull_up_8ma>
//   sdmmc_bus4 <1 RK_PD2..PD5 RK_FUNC_1 &pcfg_pull_up_8ma>
// and the R36S DTS has cd-gpios = <&gpio0 RK_PA3 GPIO_ACTIVE_LOW>. PX30 pull
// codes are the kernel's (0 none, 1 up, 2 down), drive levels 2/4/8/12 mA.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent

#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/GpioLib.h>
#include <Library/RkSdmmcPlatformLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "known.h"
#include "GpioRegs.h"

static UINT32  sysgrf[0x200 / 4], pmugrf[0x200 / 4], gpio[4][0x100 / 4];
static int     touched_other;

static UINT32 *Reg (UINTN a, int *Hiword)
{
  int  g;

  *Hiword = 1;
  if ((a >= SYS_GRF) && (a < SYS_GRF + 0x200)) return &sysgrf[(a - SYS_GRF) / 4];
  if ((a >= PMU_GRF) && (a < PMU_GRF + 0x200)) return &pmugrf[(a - PMU_GRF) / 4];
  *Hiword = 0;
  for (g = 0; g < 4; g++) {
    if ((a >= GPIO_BASE (g)) && (a < GPIO_BASE (g) + 0x100)) return &gpio[g][(a - GPIO_BASE (g)) / 4];
  }

  printf ("FAIL: unexpected access 0x%lx\n", (unsigned long)a);
  exit (1);
}

UINT32 EFIAPI MmioRead32 (UINTN a) { int h; return *Reg (a, &h); }

UINT32 EFIAPI MmioWrite32 (UINTN a, UINT32 v)
{
  int     h;
  UINT32  *r = Reg (a, &h);

  if (h) {
    UINT32  m = v >> 16;
    *r = (*r & ~m) | (v & m & 0xFFFF);
  } else {
    *r = v;
  }

  return v;
}

static UINT32 Field (UINTN Addr, UINT32 Shift, UINT32 Mask)
{
  int  h;

  return (*Reg (Addr, &h) >> Shift) & Mask;
}

static UINT32 Mux (UINT8 g, UINT8 p)   { GPIO_REG_FIELD f; GpioIomuxField (g, p, &f); return Field (f.Reg, f.Shift, f.Mask); }
static UINT32 Pull (UINT8 g, UINT8 p)  { GPIO_REG_FIELD f; GpioPullField (g, p, &f);  return Field (f.Reg, f.Shift, f.Mask); }
static UINT32 Drive (UINT8 g, UINT8 p) { GPIO_REG_FIELD f; GpioDriveField (g, p, &f); return Field (f.Reg, f.Shift, f.Mask); }

static CONST struct { CONST char *Name; UINT8 Group, Pin; UINT32 Pull; } Want[] = {
  { "sdmmc_clk",  1, GPIO_PIN_PD6, 0 },
  { "sdmmc_cmd",  1, GPIO_PIN_PD7, 1 },
  { "sdmmc_det",  0, GPIO_PIN_PA3, 1 },
  { "sdmmc_d0",   1, GPIO_PIN_PD2, 1 },
  { "sdmmc_d1",   1, GPIO_PIN_PD3, 1 },
  { "sdmmc_d2",   1, GPIO_PIN_PD4, 1 },
  { "sdmmc_d3",   1, GPIO_PIN_PD5, 1 },
};

#define DRIVE_8MA  2

static void Run (UINT32 Fill)
{
  UINT32  SysBefore[0x200 / 4], PmuBefore[0x200 / 4];
  UINTN   i, k;

  for (i = 0; i < 0x200 / 4; i++) {
    sysgrf[i] = Fill;
    pmugrf[i] = Fill;
  }

  memset (gpio, Fill ? 0xFF : 0, sizeof (gpio));
  memcpy (SysBefore, sysgrf, sizeof (sysgrf));
  memcpy (PmuBefore, pmugrf, sizeof (pmugrf));

  RkSdmmcSetIoMux ();

  for (i = 0; i < ARRAY_SIZE (Want); i++) {
    UINT32  D = Drive (Want[i].Group, Want[i].Pin);

    if ((Mux (Want[i].Group, Want[i].Pin) != 1) || (Pull (Want[i].Group, Want[i].Pin) != Want[i].Pull)) {
      printf ("FAIL: %s mux %u pull %u, want mux 1 pull %u\n", Want[i].Name, Mux (Want[i].Group, Want[i].Pin),
              Pull (Want[i].Group, Want[i].Pin), Want[i].Pull);
    }

    CHECK_EQ (Mux (Want[i].Group, Want[i].Pin), 1);
    CHECK_EQ (Pull (Want[i].Group, Want[i].Pin), Want[i].Pull);
    if (i == 0) {
      KNOWN (D == DRIVE_8MA, "sd-clk-drive",
             "%s drive level %u after RkSdmmcSetIoMux from a GRF filled with 0x%04x, the kernel and U-Boot set 8 mA "
             "(level 2); the table says GPIO_PIN_DRIVE_DEFAULT, which GpioSetIomuxConfig leaves untouched",
             Want[i].Name, D, Fill);
    } else {
      CHECK_EQ (D, DRIVE_8MA);
    }
  }

  // Card detect is an input.
  CHECK_EQ ((gpio[0][GPIO_SWPORT_DDR / 4] >> GPIO_PIN_PA3) & 1, 0);

  // No other pin changed: clear the fields the table owns and compare.
  for (i = 0; i < ARRAY_SIZE (Want); i++) {
    GPIO_REG_FIELD  F[3];

    GpioIomuxField (Want[i].Group, Want[i].Pin, &F[0]);
    GpioPullField (Want[i].Group, Want[i].Pin, &F[1]);
    GpioDriveField (Want[i].Group, Want[i].Pin, &F[2]);
    for (k = 0; k < 3; k++) {
      UINT32  *Now    = (F[k].Reg >= SYS_GRF) ? &sysgrf[(F[k].Reg - SYS_GRF) / 4] : &pmugrf[(F[k].Reg - PMU_GRF) / 4];
      UINT32  *Before = (F[k].Reg >= SYS_GRF) ? &SysBefore[(F[k].Reg - SYS_GRF) / 4] : &PmuBefore[(F[k].Reg - PMU_GRF) / 4];

      *Now    &= ~(F[k].Mask << F[k].Shift);
      *Before &= ~(F[k].Mask << F[k].Shift);
    }
  }

  CHECK (memcmp (SysBefore, sysgrf, sizeof (sysgrf)) == 0);
  CHECK (memcmp (PmuBefore, pmugrf, sizeof (pmugrf)) == 0);
}

int main (void)
{
  Run (0xFFFF);     // worst case: every field set
  Run (0x0000);

  // cd-gpios GPIO0_A3 active low: a card pulls it low.
  gpio[0][GPIO_EXT_PORT / 4] = ~(1u << GPIO_PIN_PA3);
  CHECK_EQ (RkSdmmcGetCardPresenceState (), RkSdmmcCardPresent);
  gpio[0][GPIO_EXT_PORT / 4] = 1u << GPIO_PIN_PA3;
  CHECK_EQ (RkSdmmcGetCardPresenceState (), RkSdmmcCardNotPresent);
  gpio[0][GPIO_EXT_PORT / 4] = 0;
  CHECK_EQ (RkSdmmcGetCardPresenceState (), RkSdmmcCardPresent);

  DONE_KNOWN ();
}

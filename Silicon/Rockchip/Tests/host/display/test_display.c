// Host tests for RkDisplayDxe.
//
//   test_display                 arithmetic sweeps against U-Boot's code
//   test_display <dtb>...        also, per DTB: parse, compare every
//                                computed value with U-Boot's, run the full
//                                bring-up and shutdown on the fake hardware
//                                and check the registers and their order
//   test_display --dump <dtb>    parsed panel as one JSON line (for
//                                check-display-dtbs.py)
//   test_display --log <dtb>     the driver's log for bring-up + shutdown
//
// SPDX-License-Identifier: BSD-2-Clause-Patent

#include <Uefi.h>
#include <Library/CruLib.h>
#include <Library/GpioLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "RkDisplay.h"
#include "CruPll.h"
#include "fake_env.h"
#include <libfdt.h>
#include "uboot_ref.h"
#include "test.h"

#define VOP   0xFF460000UL
#define DSI   0xFF450000UL
#define PHY   0xFF2E0000UL
#define PHY_REG(First, Second)  (PHY + ((((First) & 0x7) << 5 | ((Second) & 0x1F)) << 2))

#define DSI_GEN_HDR       (DSI + 0x6C)
#define DSI_GEN_PLD_DATA  (DSI + 0x70)
#define DSI_MODE_CFG      (DSI + 0x34)
#define VOP_SYS_CTRL2     (VOP + 0x18)
#define CRU_SOFTRST_CON3  (0xFF2B0300 + 3 * 4)
#define GPLL_HZ           1188000000UL
#define PWM_CLKSEL_DIV    11

static const char  *mFile;

static void *
ReadFile (const char *Path, long *Size)
{
  FILE  *F = fopen (Path, "rb");
  void  *Buf;

  if (F == NULL) {
    return NULL;
  }

  fseek (F, 0, SEEK_END);
  *Size = ftell (F);
  fseek (F, 0, SEEK_SET);
  Buf = malloc (*Size);
  if (fread (Buf, 1, *Size, F) != (size_t)*Size) {
    free (Buf);
    Buf = NULL;
  }

  fclose (F);
  return Buf;
}

// ---- Arithmetic sweeps -----------------------------------------------------

static void
SweepPll (void)
{
  UINT32                Rate;
  CRU_PLL_SETTINGS      Ours;
  const struct ref_pll  *Ref;
  BOOLEAN               Found;
  int                   Bad = 0, Count = 0;

  // Every CPLL rate a pixel clock from 1 MHz to 300 MHz asks for, in 1 kHz
  // steps, plus a 250 kHz sweep of the whole PLL range.
  for (Rate = 1000000; Rate <= 300000000; Rate += 1000) {
    UINT32  Hz = Rate;
    UINT32  Pll = Hz * CruVopDclkDivider (Hz);

    if ((UINT32)ref_vop_dclk_div (Hz) != CruVopDclkDivider (Hz)) {
      Bad++;
    }

    Found = CruPllFindSettings (Pll, &Ours);
    Ref   = ref_get_pll_settings (Pll);
    Count++;
    if ((Found != (Ref != NULL)) ||
        (Found && ((Ours.RefDiv != Ref->refdiv) || (Ours.FbDiv != Ref->fbdiv) ||
                   (Ours.PostDiv1 != Ref->postdiv1) || (Ours.PostDiv2 != Ref->postdiv2))))
    {
      if (Bad++ < 5) {
        printf ("PLL mismatch at %u Hz\n", Pll);
      }
    }
  }

  for (Rate = 100000000; Rate <= 1500000000; Rate += 250000) {
    Found = CruPllFindSettings (Rate, &Ours);
    Ref   = ref_get_pll_settings (Rate);
    Count++;
    if ((Found != (Ref != NULL)) ||
        (Found && ((Ours.RefDiv != Ref->refdiv) || (Ours.FbDiv != Ref->fbdiv) ||
                   (Ours.PostDiv1 != Ref->postdiv1) || (Ours.PostDiv2 != Ref->postdiv2))))
    {
      if (Bad++ < 10) {
        printf ("PLL mismatch at %u Hz\n", Rate);
      }
    }
  }

  printf ("PLL and dclk divider: %d rates, %d mismatches\n", Count, Bad);
  CHECK_EQ (Bad, 0);
}

static void
SweepDsi (void)
{
  RK_PANEL_DESC    P;
  RK_DSI_CONFIG    C;
  struct ref_mode  M;
  int              Bad = 0, Count = 0;
  UINT32           Clock, Lanes, Format, Mbps;

  memset (&P, 0, sizeof (P));
  for (Clock = 5000; Clock <= 200000; Clock += 100) {
    for (Lanes = 1; Lanes <= 4; Lanes++) {
      for (Format = 0; Format <= 3; Format++) {
        for (Mbps = 0; Mbps <= 1200; Mbps = (Clock % 5000 == 0) ? Mbps + 50 : 1201) {
          unsigned long          Rate, PhyRate;
          uint8_t                Pre = 0;
          uint16_t               Fb  = 0;
          struct ref_phy_timing  T;
          unsigned               LaneMbps;
          EFI_STATUS             S;

          memset (&M, 0, sizeof (M));
          M.clock    = Clock;
          M.hdisplay = 640; M.hsync_start = 660; M.hsync_end = 662; M.htotal = 700;
          M.vdisplay = 480; M.vsync_start = 490; M.vsync_end = 492; M.vtotal = 500;
          P.Mode.Clock      = Clock;
          P.Mode.HDisplay   = 640; P.Mode.HSyncStart = 660; P.Mode.HSyncEnd = 662; P.Mode.HTotal = 700;
          P.Mode.VDisplay   = 480; P.Mode.VSyncStart = 490; P.Mode.VSyncEnd = 492; P.Mode.VTotal = 500;
          P.Lanes        = Lanes;
          P.Format       = Format;
          P.LaneRateMbps = Mbps;

          Rate    = ref_lane_rate (&M, Format, Lanes, Mbps);
          PhyRate = ref_inno_round_rate (24000000, Rate, &Pre, &Fb);
          S       = RkDsiComputeConfig (&P, &C);
          Count++;
          if (C.LaneRateBps != Rate) {
            if (Bad++ < 5) printf ("lane rate %u kHz %u lanes fmt %u: %llu vs %lu\n", Clock, Lanes, Format, (unsigned long long)C.LaneRateBps, Rate);
            continue;
          }

          if (PhyRate == 0) {
            if (!EFI_ERROR (S)) Bad++;
            continue;
          }

          LaneMbps = PhyRate / 1000 / 1000;
          ref_inno_timings (PhyRate, &T);
          if (EFI_ERROR (S) || (C.PhyRateBps != PhyRate) || (C.PhyPrediv != Pre) || (C.PhyFbdiv != Fb) ||
              (C.LaneMbps != LaneMbps) || (C.EscClkDiv != ref_esc_clk_div (LaneMbps)) ||
              (C.HlineLbcc != (UINT32)ref_lbcc (700, LaneMbps, Clock)) ||
              (C.HsaLbcc != (UINT32)ref_lbcc (2, LaneMbps, Clock)) ||
              (C.HbpLbcc != (UINT32)ref_lbcc (38, LaneMbps, Clock)) ||
              (C.Vsa != 2) || (C.Vfp != 10) || (C.Vbp != 8) ||
              (C.HsExit != T.hs_exit) || (C.ClkPost != T.clk_post) || (C.ClkPre != T.clk_pre) ||
              (C.Lpx != T.lpx) || (C.TaGo != T.ta_go) || (C.TaSure != T.ta_sure) || (C.TaWait != T.ta_wait) ||
              (C.HsPrepare != T.hs_prepare) || (C.HsTrail != T.hs_trail) || (C.ClkHsZero != T.clk_lane_hs_zero) ||
              (C.DataHsZero != T.data_lane_hs_zero) || (C.Wakeup != T.wakeup))
          {
            if (Bad++ < 5) printf ("DSI config mismatch at %u kHz %u lanes fmt %u lane-rate %u\n", Clock, Lanes, Format, Mbps);
          }
        }
      }
    }
  }

  printf ("DSI lane rate, PHY PLL, PHY timings, lbcc: %d cases, %d mismatches\n", Count, Bad);
  CHECK_EQ (Bad, 0);
}

static void
SweepSmall (void)
{
  int      Bad = 0, Count = 0;
  unsigned Type, Len, Ch;
  uint8_t  Buf[300];
  UINT64   Clk;
  UINT32   Ns;

  for (Len = 0; Len < sizeof (Buf); Len++) {
    Buf[Len] = (uint8_t)(Len * 37 + 11);
  }

  for (Type = 0; Type < 256; Type++) {
    for (Len = 0; Len < 260; Len++) {
      uint8_t  Ours = RkPanelCmdMsgType ((UINT8)Type, (UINT8)Len);
      uint8_t  Ref  = ref_cmd_msg_type ((uint8_t)Type, (uint8_t)Len);

      Count++;
      if (Ours != Ref) {
        Bad++;
        continue;
      }

      for (Ch = 0; (Ref != 0) && (Ch < 4); Ch++) {
        if (RkDsiPacketHeader ((UINT8)Ch, Ref, Buf, Len) != ref_packet_header (Ch, Ref, Buf, Len)) {
          Bad++;
        }
      }
    }
  }

  for (Clk = 1000000; Clk <= 1200000000; Clk = Clk * 3 / 2 + 7) {
    for (Ns = 0; Ns <= 2000000; Ns = Ns * 2 + 13) {
      Count++;
      if (RkPwmCount (Clk, Ns) != (UINT32)ref_pwm_count (Clk, Ns)) {
        Bad++;
      }
    }
  }

  printf ("Packet types and headers, PWM counts: %d cases, %d mismatches\n", Count, Bad);
  CHECK_EQ (Bad, 0);
}

// ---- One DTB ---------------------------------------------------------------

static void
RefModeOf (const RK_PANEL_DESC *P, struct ref_mode *M)
{
  struct ref_dt_timing  T;

  T.hactive = P->Timing.HActive; T.vactive = P->Timing.VActive; T.pixelclock = P->Timing.ClockFrequency;
  T.hsync_len = P->Timing.HSyncLen; T.hfront_porch = P->Timing.HFrontPorch; T.hback_porch = P->Timing.HBackPorch;
  T.vsync_len = P->Timing.VSyncLen; T.vfront_porch = P->Timing.VFrontPorch; T.vback_porch = P->Timing.VBackPorch;
  T.hsync_active = P->Timing.HSyncActive; T.vsync_active = P->Timing.VSyncActive;
  T.pixelclk_active = P->Timing.PixelClkActive; T.screen_rotate = P->Timing.ScreenRotate;
  ref_mode_from_timing (&T, M);
}

static void
DumpPanel (const RK_PANEL_DESC *P, EFI_STATUS Status)
{
  const RK_BACKLIGHT_DESC  *B = &P->Backlight;
  RK_PANEL_CMD             Cmd;
  UINT32                   Off = 0, Count = 0;

  while (RkPanelNextCmd (P->InitSeq, P->InitSeqLen, &Off, &Cmd)) {
    Count++;
  }

  printf ("{\"file\": \"%s\", \"status\": %lld", mFile, (long long)(Status & 0xFF));
  if (EFI_ERROR (Status)) {
    printf ("}\n");
    return;
  }

  printf (", \"panel\": \"%s\", \"crtc\": %u, \"lanes\": %u, \"format\": %u, \"flags\": %u, \"channel\": %u, \"lane_rate\": %u",
    P->PanelNodeName, P->CrtcId, P->Lanes, P->Format, P->ModeFlags, P->Channel, P->LaneRateMbps);
  printf (", \"delays\": [%u, %u, %u, %u, %u, %u], \"bus_format\": %u, \"power_invert\": %d",
    P->PrepareDelayMs, P->ResetDelayMs, P->InitDelayMs, P->EnableDelayMs, P->DisableDelayMs, P->UnprepareDelayMs,
    P->BusFormat, P->PowerInvert);
  printf (", \"timing\": [%u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u], \"ntimings\": %u",
    P->Timing.ClockFrequency, P->Timing.HActive, P->Timing.HFrontPorch, P->Timing.HSyncLen, P->Timing.HBackPorch,
    P->Timing.VActive, P->Timing.VFrontPorch, P->Timing.VSyncLen, P->Timing.VBackPorch,
    P->Timing.HSyncActive, P->Timing.VSyncActive, P->Timing.PixelClkActive, P->TimingCount);
  printf (", \"init_cmds\": %u, \"init_len\": %u, \"exit_len\": %u", Count, P->InitSeqLen, P->ExitSeqLen);
  printf (", \"reset\": [%d, %u, %u, %d], \"enable\": [%d, %u, %u, %d]",
    P->ResetGpio.Valid, P->ResetGpio.Bank, P->ResetGpio.Pin, P->ResetGpio.ActiveLow,
    P->EnableGpio.Valid, P->EnableGpio.Bank, P->EnableGpio.Pin, P->EnableGpio.ActiveLow);
  printf (", \"power\": [%d, \"%s\", %u, %d], \"blpower\": [%d, \"%s\", %u, %d]",
    P->PowerSupply.Present, P->PowerSupply.Present ? P->PowerSupply.Name : "", P->PowerSupply.MinMicroVolts, P->PowerSupply.Fixed,
    P->BacklightSupply.Present, P->BacklightSupply.Present ? P->BacklightSupply.Name : "", P->BacklightSupply.MinMicroVolts, P->BacklightSupply.Fixed);
  printf (", \"bl\": [%d, \"0x%lx\", %u, %u, %u, %d, %u, %u, %d, %d, %u]}\n",
    B->Present, (unsigned long)B->PwmBase, B->PwmBlock, B->Channel, B->PeriodNs, B->Inverted, B->DefaultLevel, B->MaxLevel,
    B->Enable.Valid, B->Supply.Present, B->PinCount);
}

// Level of a GPIO output after trace entry Upto (exclusive), from DR writes.
static int
GpioLevelAt (UINT8 Bank, UINT8 Pin, int Upto)
{
  UINTN  Dr = GPIO_BASE (Bank);
  int    i, Level = 0;

  for (i = 0; i < Upto; i++) {
    if (fake_trace[i].Addr == Dr) {
      Level = (fake_trace[i].After >> Pin) & 1;
    }
  }

  return Level;
}

// Trace index where the GPIO goes to its active level for the first time
// at or after From, or -1.
static int
GpioActiveAt (const RK_GPIO_SPEC *G, int From, BOOLEAN Active)
{
  UINTN  Dr = GPIO_BASE (G->Bank);
  int    i, Want = (Active ? 1 : 0) ^ (G->ActiveLow ? 1 : 0);

  for (i = From; i < fake_trace_count; i++) {
    if ((fake_trace[i].Addr == Dr) && (((fake_trace[i].After >> G->Pin) & 1) == Want) &&
        ((i == 0) || (GpioLevelAt (G->Bank, G->Pin, i) != Want)))
    {
      return i;
    }
  }

  return -1;
}

static int
FirstWrite (UINTN Addr, int From)
{
  int  i;

  for (i = From; i < fake_trace_count; i++) {
    if (fake_trace[i].Addr == Addr) {
      return i;
    }
  }

  return -1;
}

static int
LastWrite (UINTN Addr)
{
  int  i;

  for (i = fake_trace_count - 1; i >= 0; i--) {
    if (fake_trace[i].Addr == Addr) {
      return i;
    }
  }

  return -1;
}

// The commands as U-Boot would send them, against the GEN_HDR and
// GEN_PLD_DATA writes from trace entry From on. Returns the index after the
// last command's header, or -1.
static int
CheckCmds (const RK_PANEL_DESC *P, const UINT8 *Seq, UINT32 Len, int From, int *Sent)
{
  struct ref_cmd  Cmds[512];
  int             n, i, t = From, LastHdr = From;
  UINT64          LastTime = 0;
  int             LastDelay = 0;

  n = ref_parse_cmds (Seq, (int)Len, Cmds, 512);
  CHECK (n >= 0);
  *Sent = 0;
  for (i = 0; i < n; i++) {
    uint8_t   Type = ref_cmd_msg_type (Cmds[i].type, Cmds[i].len);
    uint32_t  Words[80];
    int       w, nw = 0;

    CHECK (Type != 0);
    if ((Type == 0x29) || (Type == 0x39)) {
      for (w = 0; w < Cmds[i].len; w += 4) {
        uint32_t  v = 0;
        memcpy (&v, Cmds[i].payload + w, (Cmds[i].len - w) < 4 ? (Cmds[i].len - w) : 4);
        Words[nw++] = v;
      }
    }

    // Payload words first, then the header.
    for (w = 0; w < nw; w++) {
      while (t < fake_trace_count && fake_trace[t].Addr != DSI_GEN_PLD_DATA && fake_trace[t].Addr != DSI_GEN_HDR) t++;
      if (t >= fake_trace_count || fake_trace[t].Addr != DSI_GEN_PLD_DATA || fake_trace[t].Value != Words[w]) {
        printf ("  %s: command %d payload word %d differs\n", mFile, i, w);
        return -1;
      }

      t++;
    }

    while (t < fake_trace_count && fake_trace[t].Addr != DSI_GEN_PLD_DATA && fake_trace[t].Addr != DSI_GEN_HDR) t++;
    if (t >= fake_trace_count || fake_trace[t].Addr != DSI_GEN_HDR ||
        fake_trace[t].Value != ref_packet_header (P->Channel, Type, Cmds[i].payload, Cmds[i].len))
    {
      printf ("  %s: command %d header differs (0x%x)\n", mFile, i, t < fake_trace_count ? fake_trace[t].Value : 0);
      return -1;
    }

    // The previous command's delay has passed before this one goes out.
    if ((i > 0) && (fake_trace[t].Time - LastTime < (UINT64)LastDelay * 1000)) {
      printf ("  %s: command %d sent %llu us after the previous one, wants %d ms\n", mFile, i,
        (unsigned long long)(fake_trace[t].Time - LastTime), LastDelay);
      return -1;
    }

    LastTime  = fake_trace[t].Time;
    LastDelay = Cmds[i].delay_ms;
    LastHdr   = ++t;
    (*Sent)++;
  }

  return LastHdr;
}

static void
SetUpHardware (void)
{
  fake_reset ();
  fake_pmic_count = 0;
  // GPLL 1188 MHz in normal mode (U-Boot's), pwm clocks GPLL / 12
  fake_set (0xFF2BC000, (1 << 12) | 99);
  fake_set (0xFF2BC004, (1 << 12) | (1 << 6) | 2);
  fake_set (0xFF2BC020, 1);
  fake_set (0xFF2B0100 + 52 * 4, (PWM_CLKSEL_DIV << 8) | PWM_CLKSEL_DIV);
  // VOP in standby, as after reset
  fake_set (VOP_SYS_CTRL2, BIT1);
}

static void
TestDtb (const char *Path, int Mode)
{
  long                   Size;
  void                   *Fdt;
  static RK_DISPLAY      D;
  RK_PANEL_DESC          *P = &D.Panel;
  EFI_STATUS             Status;
  struct ref_mode        M;
  struct ref_vop         RV;
  unsigned long          Rate, PhyRate, PwmClk;
  uint8_t                Pre = 0;
  uint16_t               Fb = 0;
  struct ref_phy_timing  T;
  unsigned               LaneMbps;
  int                    Div, Part, Sent, ExitSent, i;
  const struct ref_pll   *Pll;
  int                    tPower, tResetOn, tResetOff, tFirstCmd, tAfterCmds, tVideo, tVopOn, tPwm, tStart;
  uint32_t               Levels[256];
  int                    NLevels = 0, Fails = g_fails;

  mFile = Path;
  Fdt   = ReadFile (Path, &Size);
  if (Fdt == NULL) {
    printf ("cannot read %s\n", Path);
    g_fails++;
    return;
  }

  memset (&D, 0, sizeof (D));
  fake_verbose = (Mode == 2);
  Status       = RkPanelParse (Fdt, P);
  if (Mode == 1) {
    DumpPanel (P, Status);
    return;
  }

  CHECK (!EFI_ERROR (Status));
  if (EFI_ERROR (Status)) {
    printf ("  %s: parse failed\n", Path);
    return;
  }

  if (Mode == 2) {
    RkPanelLog (P);
  }

  // Mode, timing words and every computed value against U-Boot's code
  RefModeOf (P, &M);
  CHECK_EQ (P->Mode.HDisplay, M.hdisplay); CHECK_EQ (P->Mode.HSyncStart, M.hsync_start);
  CHECK_EQ (P->Mode.HSyncEnd, M.hsync_end); CHECK_EQ (P->Mode.HTotal, M.htotal);
  CHECK_EQ (P->Mode.VDisplay, M.vdisplay); CHECK_EQ (P->Mode.VSyncStart, M.vsync_start);
  CHECK_EQ (P->Mode.VSyncEnd, M.vsync_end); CHECK_EQ (P->Mode.VTotal, M.vtotal);
  CHECK_EQ (P->Mode.Clock, M.clock); CHECK_EQ (P->Mode.Flags, M.flags);

  Rate     = ref_lane_rate (&M, P->Format, P->Lanes, P->LaneRateMbps);
  PhyRate  = ref_inno_round_rate (24000000, Rate, &Pre, &Fb);
  LaneMbps = PhyRate / 1000 / 1000;
  ref_inno_timings (PhyRate, &T);
  ref_vop_config (&M, M.hdisplay, M.vdisplay, 32, &RV);
  Div = ref_vop_dclk_div (M.clock * 1000);
  Pll = ref_get_pll_settings ((unsigned long)M.clock * 1000 * Div);
  CHECK (Pll != NULL);

  // Bring-up on the fake hardware, 32 bpp framebuffer
  SetUpHardware ();
  D.FbBase   = 0x3E000000;
  D.FbWidth  = P->Mode.HDisplay;
  D.FbHeight = P->Mode.VDisplay;
  D.FbStride = D.FbWidth * 4;
  D.FbBpp    = 32;
  Status     = RkDisplayEnable (&D);
  CHECK (!EFI_ERROR (Status));
  CHECK_EQ (fake_errors, 0);

  // CRU: CPLL and the dclk divider where U-Boot puts them
  if (Pll != NULL) {
    CHECK_EQ (fake_get (0xFF2B0040) & 0xFFF, Pll->fbdiv);
    CHECK_EQ ((fake_get (0xFF2B0040) >> 12) & 7, Pll->postdiv1);
    CHECK_EQ (fake_get (0xFF2B0044) & 0x3F, Pll->refdiv);
    CHECK_EQ ((fake_get (0xFF2B0044) >> 6) & 7, Pll->postdiv2);
    CHECK_EQ ((fake_get (0xFF2B00A0) >> 4) & 3, 1);             // CPLL normal mode
  }

  CHECK_EQ (fake_get (0xFF2B0114) & 0xFF, Div - 1);
  CHECK_EQ ((fake_get (0xFF2B0114) >> 11) & 1, 0);
  CHECK_EQ ((fake_get (0xFF2B0114) >> 14) & 3, 0);

  // VOP
  CHECK_EQ (fake_get (VOP + 0x100), RV.htotal_pw);
  CHECK_EQ (fake_get (VOP + 0x104), RV.hact_st_end);
  CHECK_EQ (fake_get (VOP + 0x108), RV.vtotal_pw);
  CHECK_EQ (fake_get (VOP + 0x10C), RV.vact_st_end);
  CHECK_EQ (fake_get (VOP + 0x30) & 0xFFF, RV.line_flag);
  CHECK_EQ ((fake_get (VOP + 0x20) >> 24) & 1, 1);
  CHECK_EQ ((fake_get (VOP + 0x20) >> 26) & 7, RV.pin_pol);
  CHECK_EQ ((fake_get (VOP + 0x20) >> 25) & 1, RV.dclk_inv);
  CHECK_EQ ((fake_get (VOP + 0x28) >> 6) & 7, ref_dither_down (P->BusFormat));
  CHECK_EQ ((fake_get (VOP + 0x28) >> 14) & 1, 0);
  CHECK_EQ ((fake_get (VOP_SYS_CTRL2) >> 1) & 1, 0);
  CHECK_EQ ((fake_get (VOP_SYS_CTRL2) >> 13) & 1, 1);
  CHECK_EQ (fake_get (VOP + 0x90) & 0x1071, 1);                 // ARGB8888, no rb swap, on
  CHECK_EQ (fake_get (VOP + 0x98) & 0x1FFF, RV.vir);
  CHECK_EQ (fake_get (VOP + 0xA0), 0x3E000000);
  CHECK_EQ (fake_get (VOP + 0xA4), RV.dsp_info);
  CHECK_EQ (fake_get (VOP + 0xA8), RV.dsp_st);
  CHECK_EQ (fake_get (VOP + 0x00) & 1, 1);

  // DSI host
  CHECK_EQ (fake_get (DSI + 0x08), (10 << 8) | ref_esc_clk_div (LaneMbps));
  CHECK_EQ (fake_get (DSI + 0x0C), P->Channel);
  CHECK_EQ (fake_get (DSI + 0x10), ref_dpi_color (P->Format));
  CHECK_EQ (fake_get (DSI + 0x14), ref_dpi_pol (M.flags));
  CHECK_EQ (fake_get (DSI + 0x18), (4 << 16) | 4);
  CHECK_EQ (fake_get (DSI + 0x2C), ref_pckhdl (P->ModeFlags));
  CHECK_EQ (fake_get (DSI + 0x38) & ~BIT15, ref_vid_mode_cfg (P->ModeFlags));
  CHECK_EQ (fake_get (DSI + 0x3C), M.hdisplay);
  CHECK_EQ (fake_get (DSI + 0x50), ref_lbcc (M.htotal, LaneMbps, M.clock));
  CHECK_EQ (fake_get (DSI + 0x48), ref_lbcc (M.hsync_end - M.hsync_start, LaneMbps, M.clock));
  CHECK_EQ (fake_get (DSI + 0x4C), ref_lbcc (M.htotal - M.hsync_end, LaneMbps, M.clock));
  CHECK_EQ (fake_get (DSI + 0x54), M.vsync_end - M.vsync_start);
  CHECK_EQ (fake_get (DSI + 0x58), M.vtotal - M.vsync_end);
  CHECK_EQ (fake_get (DSI + 0x5C), M.vsync_start - M.vdisplay);
  CHECK_EQ (fake_get (DSI + 0x60), M.vdisplay);
  CHECK_EQ (fake_get (DSI + 0x78), (1000 << 16) | 1000);
  CHECK_EQ (fake_get (DSI + 0x8C), 0xD00);
  CHECK_EQ (fake_get (DSI + 0x9C), (0x14u << 24) | (0x10 << 16) | 10000);
  CHECK_EQ (fake_get (DSI + 0x98), (0x40 << 16) | 0x40);
  CHECK_EQ (fake_get (DSI + 0xA4), (0x20 << 8) | (P->Lanes - 1));
  CHECK_EQ (fake_get (DSI + 0xA0), 0x7);                         // enableclk, rstz, shutdownz
  CHECK_EQ (fake_get (DSI + 0x04), 1);
  CHECK_EQ (fake_get (DSI_MODE_CFG) & 1, (P->ModeFlags & RK_DSI_MODE_VIDEO) ? 0 : 1);
  CHECK_EQ (fake_get (DSI + 0x94) & 1, 1);                       // HS clock requested
  CHECK_EQ ((fake_get (DSI + 0x94) >> 1) & 1, (P->ModeFlags & RK_DSI_CLOCK_NON_CONTINUOUS) ? 1 : 0);
  CHECK_EQ ((fake_get (DSI + 0x38) >> 15) & 1, (P->ModeFlags & RK_DSI_MODE_LPM) ? 1 : 0);
  CHECK_EQ (fake_get (0xFF140438) & 1, 0);                       // VOPSEL: VOPB

  // Inno PHY
  CHECK_EQ (fake_get (PHY_REG (0, 0x03)) & 0x1F, Pre);
  CHECK_EQ ((fake_get (PHY_REG (0, 0x03)) >> 5) & 1, Fb >> 8);
  CHECK_EQ (fake_get (PHY_REG (0, 0x04)) & 0xFF, Fb & 0xFF);
  CHECK_EQ (fake_get (PHY_REG (0, 0x00)) & 0xFF, 0x7D);          // lanes on, power work, bandgap on
  CHECK_EQ (fake_get (PHY_REG (0, 0x01)) & 0x7, 0);
  CHECK_EQ (fake_get (PHY_REG (1, 0x00)) & 1, 1);
  CHECK_EQ (fake_get (PHY_REG (7, 0x03)) & 7, 1);
  for (Part = 2; Part <= 6; Part++) {
    CHECK_EQ (fake_get (PHY_REG (Part, 0x05)), T.lpx & 0x3F);
    CHECK_EQ (fake_get (PHY_REG (Part, 0x06)), T.hs_prepare & 0x7F);
    CHECK_EQ (fake_get (PHY_REG (Part, 0x07)), (Part == 2 ? T.clk_lane_hs_zero : T.data_lane_hs_zero) & 0x3F);
    CHECK_EQ (fake_get (PHY_REG (Part, 0x08)), T.hs_trail & 0x7F);
    CHECK_EQ (fake_get (PHY_REG (Part, 0x09)), T.hs_exit & 0x1F);
    CHECK_EQ (fake_get (PHY_REG (Part, 0x0A)), T.clk_post & 0xF);
    CHECK_EQ (fake_get (PHY_REG (Part, 0x0E)), T.clk_pre & 0xF);
    CHECK_EQ (fake_get (PHY_REG (Part, 0x0C)), (T.wakeup >> 8) & 3);
    CHECK_EQ (fake_get (PHY_REG (Part, 0x0D)), T.wakeup & 0xFF);
    CHECK_EQ (fake_get (PHY_REG (Part, 0x10)), T.ta_go & 0x3F);
    CHECK_EQ (fake_get (PHY_REG (Part, 0x11)), T.ta_sure & 0x3F);
    CHECK_EQ (fake_get (PHY_REG (Part, 0x12)), T.ta_wait & 0x3F);
  }

  // Panel: rails, then the reset pulse, then the commands in U-Boot's
  // packets and with their delays; VOP out of standby, video mode and the
  // backlight only after that.
  tPower = (fake_pmic_count > 0) ? fake_pmic[0].Trace : 0;
  if (P->PowerSupply.Present && !P->PowerSupply.Fixed) {
    CHECK (fake_pmic_count >= 1);
    CHECK (fake_pmic_count >= 1 && strcmp (fake_pmic[0].Name, fdt_get_name (Fdt, P->PowerSupply.Node, NULL)) == 0);
    CHECK (fake_pmic_count >= 1 && fake_pmic[0].Enable == !P->PowerInvert);
  }

  tStart    = 0;
  tResetOn  = P->ResetGpio.Valid ? GpioActiveAt (&P->ResetGpio, tPower, TRUE) : 0;
  tResetOff = P->ResetGpio.Valid ? GpioActiveAt (&P->ResetGpio, tResetOn + 1, FALSE) : 0;
  tFirstCmd = FirstWrite (DSI_GEN_HDR, 0);
  if (P->ResetGpio.Valid) {
    CHECK (tResetOn >= tPower);
    CHECK (tResetOff > tResetOn);
    CHECK (tResetOn >= 0 && tResetOff >= 0 && fake_trace[tResetOff].Time - fake_trace[tResetOn].Time >= (UINT64)P->ResetDelayMs * 1000);
    if (tFirstCmd >= 0) {
      CHECK (tFirstCmd > tResetOff);
      CHECK (fake_trace[tFirstCmd].Time - fake_trace[tResetOff].Time >= (UINT64)P->InitDelayMs * 1000);
    }
  }

  if (P->EnableGpio.Valid) {
    CHECK_EQ (GpioLevelAt (P->EnableGpio.Bank, P->EnableGpio.Pin, fake_trace_count), P->EnableGpio.ActiveLow ? 0 : 1);
  }

  if (P->ResetGpio.Valid) {
    CHECK_EQ (GpioLevelAt (P->ResetGpio.Bank, P->ResetGpio.Pin, fake_trace_count), P->ResetGpio.ActiveLow ? 1 : 0);
  }

  tAfterCmds = CheckCmds (P, P->InitSeq, P->InitSeqLen, tStart, &Sent);
  CHECK (tAfterCmds >= 0);
  tVopOn = -1;
  for (i = 0; i < fake_trace_count; i++) {
    if ((fake_trace[i].Addr == VOP_SYS_CTRL2) && ((fake_trace[i].After & BIT1) == 0)) {
      tVopOn = i;
      break;
    }
  }

  tVideo = LastWrite (DSI_MODE_CFG);
  CHECK (tVopOn > LastWrite (DSI_GEN_HDR));
  CHECK (tVideo > tVopOn);

  // Backlight
  if (P->Backlight.Present) {
    UINTN  Pwm = P->Backlight.PwmBase;
    int    Index;
    const  uint32_t *Cell;
    int    Bl;

    PwmClk = GPLL_HZ / (PWM_CLKSEL_DIV + 1);
    Bl     = fdt_node_offset_by_compatible (Fdt, -1, "pwm-backlight");
    {
      const fdt32_t  *Def = Bl >= 0 ? fdt_getprop (Fdt, Bl, "default-brightness-level", NULL) : NULL;
      int            L = 0;

      Index   = Def ? (int)fdt32_to_cpu (*Def) : 255;
      Cell    = Bl >= 0 ? fdt_getprop (Fdt, Bl, "brightness-levels", &L) : NULL;
      NLevels = Cell ? L / 4 : 0;
    }
    for (i = 0; i < NLevels && i < 256; i++) Levels[i] = fdt32_to_cpu (Cell[i]);
    CHECK_EQ (D.PwmClockHz, PwmClk);
    CHECK_EQ (fake_get (Pwm + 4), ref_pwm_count (PwmClk, P->Backlight.PeriodNs));
    CHECK_EQ (fake_get (Pwm + 8), ref_pwm_count (PwmClk, ref_backlight_duty (P->Backlight.PeriodNs, Cell ? Levels : NULL, NLevels, Index)));
    CHECK_EQ (fake_get (Pwm + 0xC) & 0x17F, 0x3 | (P->Backlight.Inverted ? BIT4 : BIT3));
    tPwm = FirstWrite (Pwm + 0xC, 0);
    CHECK (tPwm > tVideo);
    CHECK (fake_trace[tPwm].Time - fake_trace[tVideo].Time >= (UINT64)P->EnableDelayMs * 1000);
  } else {
    printf ("  %s: no backlight\n", Path);
  }

  // Shutdown
  {
    int  tShut = fake_trace_count, tExit;

    RkDisplayShutdown (&D);
    CHECK_EQ (fake_errors, 0);
    CHECK_EQ ((fake_get (VOP_SYS_CTRL2) >> 1) & 1, 1);
    CHECK_EQ (fake_get (DSI + 0x04), 0);
    CHECK_EQ (fake_get (DSI + 0xA0), 0);
    CHECK_EQ (fake_get (PHY_REG (0, 0x01)) & 3, 3);
    CHECK_EQ (fake_get (PHY_REG (0, 0x00)) & 0xFF, 0x82);
    CHECK_EQ (fake_get (CRU_SOFTRST_CON3) & 0x6030, 0);           // resets released again
    CHECK (FirstWrite (CRU_SOFTRST_CON3, tShut) >= 0);
    if (P->Backlight.Present) {
      CHECK_EQ (fake_get (P->Backlight.PwmBase + 8), 0);
    }

    tExit = CheckCmds (P, P->ExitSeq, P->ExitSeqLen, tShut, &ExitSent);
    CHECK (tExit >= 0);
    // The exit commands go out after the VOP stopped.
    if (ExitSent > 0) {
      CHECK (FirstWrite (VOP_SYS_CTRL2, tShut) < tExit);
    }
    if (P->ResetGpio.Valid) {
      CHECK_EQ (GpioLevelAt (P->ResetGpio.Bank, P->ResetGpio.Pin, fake_trace_count), P->ResetGpio.ActiveLow ? 1 : 0);
    }

    if (P->EnableGpio.Valid) {
      CHECK_EQ (GpioLevelAt (P->EnableGpio.Bank, P->EnableGpio.Pin, fake_trace_count), P->EnableGpio.ActiveLow ? 1 : 0);
    }

    if (P->PowerSupply.Present && !P->PowerSupply.Fixed) {
      int  k, Off = 0;

      for (k = 0; k < fake_pmic_count; k++) {
        if ((fake_pmic[k].Trace >= tShut) && (strcmp (fake_pmic[k].Name, fdt_get_name (Fdt, P->PowerSupply.Node, NULL)) == 0) &&
            (fake_pmic[k].Enable == P->PowerInvert))
        {
          Off = 1;
        }
      }

      CHECK (Off);
    }
  }

  if (g_fails == Fails) {
    printf ("ok %s: %s %ux%u %u kHz, %u lanes %lu Mbps, CPLL %lu Hz / %d, %d+%d cmds\n", Path, P->PanelNodeName,
      P->Mode.HDisplay, P->Mode.VDisplay, P->Mode.Clock, P->Lanes, (unsigned long)LaneMbps,
      Pll ? ref_pll_rate (Pll) : 0, Div, Sent, ExitSent);
  } else {
    printf ("FAILED %s\n", Path);
  }

  free (Fdt);
}

int
main (int argc, char **argv)
{
  int  i;

  if ((argc == 3) && (strcmp (argv[1], "--dump") == 0)) {
    TestDtb (argv[2], 1);
    return 0;
  }

  if ((argc == 3) && (strcmp (argv[1], "--log") == 0)) {
    TestDtb (argv[2], 2);
    DONE ();
  }

  SweepPll ();
  SweepDsi ();
  SweepSmall ();
  for (i = 1; i < argc; i++) {
    TestDtb (argv[i], 0);
  }

  DONE ();
}

/** @file
  The arithmetic of the display bring-up, without register access: timing
  to mode, DSI lane rate, Inno PHY PLL and lane timings, DSI line timing in
  lane byte clocks, VOP timing words, PWM counts, DSI packet headers.

  Ported from U-Boot drivers/video/drm (rockchip_display.c
  display_get_timing_from_dts, dw_mipi_dsi.c, inno_video_combo_phy.c,
  rockchip_vop.c, drm_mipi_dsi.c) and drivers/video/pwm_backlight.c,
  drivers/pwm/rk_pwm.c. The integer types follow U-Boot's where truncation
  could change a result. Tests/host/display checks every function here
  against the U-Boot originals for each panel the andr36oid DTBs describe.

  SPDX-License-Identifier: GPL-2.0+
**/

#include "RkDisplay.h"

#define PSEC_PER_SEC        1000000000000ULL
#define DSI_MAX_LANE_BPS    1000000000ULL      // px30 max_bit_rate_per_lane
#define INNO_MAX_FOUT       1000000000ULL

#define DIV_ROUND_UP_U32(n, d)  (((n) + (d) - 1) / (d))

VOID
RkModeFromTiming (
  IN  CONST RK_DT_TIMING  *Timing,
  OUT RK_DISPLAY_MODE     *Mode
  )
{
  UINT32  Flags;

  Flags  = Timing->HSyncActive ? RK_MODE_PHSYNC : RK_MODE_NHSYNC;
  Flags |= Timing->VSyncActive ? RK_MODE_PVSYNC : RK_MODE_NVSYNC;
  Flags |= Timing->PixelClkActive ? RK_MODE_PPIXDATA : 0;
  if (Timing->ScreenRotate == RK_MODE_XMIRROR) {
    Flags |= RK_MODE_XMIRROR;
  } else if (Timing->ScreenRotate == RK_MODE_YMIRROR) {
    Flags |= RK_MODE_YMIRROR;
  } else if (Timing->ScreenRotate == (RK_MODE_XMIRROR | RK_MODE_YMIRROR)) {
    Flags |= RK_MODE_XMIRROR | RK_MODE_YMIRROR;
  }

  Mode->HDisplay   = Timing->HActive;
  Mode->HSyncStart = Mode->HDisplay + Timing->HFrontPorch;
  Mode->HSyncEnd   = Mode->HSyncStart + Timing->HSyncLen;
  Mode->HTotal     = Mode->HSyncEnd + Timing->HBackPorch;
  Mode->VDisplay   = Timing->VActive;
  Mode->VSyncStart = Mode->VDisplay + Timing->VFrontPorch;
  Mode->VSyncEnd   = Mode->VSyncStart + Timing->VSyncLen;
  Mode->VTotal     = Mode->VSyncEnd + Timing->VBackPorch;
  Mode->Clock      = Timing->ClockFrequency / 1000;
  Mode->Flags      = Flags;
}

UINT32
RkDsiFormatBpp (
  IN UINT32  Format
  )
{
  switch (Format) {
    case RK_DSI_FMT_RGB888:
    case RK_DSI_FMT_RGB666:
      return 24;
    case RK_DSI_FMT_RGB666_PACKED:
      return 18;
    case RK_DSI_FMT_RGB565:
      return 16;
    default:
      return 24;       // U-Boot: bpp < 0 falls back to 24
  }
}

/**
  dw_mipi_dsi_get_lane_rate: pixel bandwidth per lane plus 1/9 headroom,
  capped at 1 Gbps, unless the host node sets rockchip,lane-rate (Mbps).
**/
UINT64
RkDsiLaneRate (
  IN CONST RK_DISPLAY_MODE  *Mode,
  IN UINT32                 Format,
  IN UINT32                 Lanes,
  IN UINT32                 LaneRateMbps
  )
{
  UINT64  Tmp;

  if (LaneRateMbps > 0) {
    return (UINT32)(LaneRateMbps * 1000 * 1000);
  }

  if (Lanes == 0) {
    return 0;
  }

  Tmp  = (UINT64)Mode->Clock * 1000 * RkDsiFormatBpp (Format);
  Tmp /= Lanes;
  Tmp *= 10;
  Tmp /= 9;

  return (Tmp > DSI_MAX_LANE_BPS) ? DSI_MAX_LANE_BPS : Tmp;
}

/**
  inno_video_phy_pll_round_rate: Fout = Fref / prediv * fbdiv with
  Fref = parent / 2, 5 MHz <= Fref / prediv <= 40 MHz, fbdiv 12..511 but 15.
  Returns 0 when nothing fits.
**/
UINT64
RkInnoPllRoundRate (
  IN  UINT64  ParentRate,
  IN  UINT64  Rate,
  OUT UINT8   *Prediv,
  OUT UINT16  *Fbdiv
  )
{
  UINT64  Fref;
  UINT64  Fout;
  UINT64  BestFreq;
  UINT8   MinPrediv;
  UINT8   MaxPrediv;
  UINT8   CurPrediv;
  UINT8   BestPrediv;
  UINT16  CurFbdiv;
  UINT16  BestFbdiv;
  UINT32  MinDelta;
  UINT32  Delta;
  UINT64  Tmp;
  INT64   Diff;

  BestFreq   = 0;
  BestPrediv = 1;
  BestFbdiv  = 1;
  MinDelta   = 0xFFFFFFFF;

  Fref = ParentRate / 2;
  Fout = (Rate > INNO_MAX_FOUT) ? INNO_MAX_FOUT : Rate;

  MinPrediv = (UINT8)((Fref + 40000000 - 1) / 40000000);
  MaxPrediv = (UINT8)(Fref / 5000000);

  for (CurPrediv = MinPrediv; CurPrediv <= MaxPrediv; CurPrediv++) {
    Tmp      = Fout * CurPrediv / Fref;
    CurFbdiv = (UINT16)Tmp;

    // Feedback divider settings: 12, 13, 14, 16 .. 511
    if (CurFbdiv == 15) {
      continue;
    }

    if ((CurFbdiv < 12) || (CurFbdiv > 511)) {
      continue;
    }

    Tmp   = (UINT64)CurFbdiv * Fref / CurPrediv;
    Diff  = (INT64)(Fout - Tmp);
    Delta = (UINT32)(Diff < 0 ? -Diff : Diff);
    if (Delta == 0) {
      BestPrediv = CurPrediv;
      BestFbdiv  = CurFbdiv;
      BestFreq   = Tmp;
      break;
    } else if (Delta < MinDelta) {
      BestPrediv = CurPrediv;
      BestFbdiv  = CurFbdiv;
      BestFreq   = Tmp;
      MinDelta   = Delta;
    }
  }

  if (BestFreq != 0) {
    *Prediv = BestPrediv;
    *Fbdiv  = BestFbdiv;
  }

  return BestFreq;
}

/**
  inno_video_phy_mipi_mode_enable's counters: the D-PHY minimum times in
  units of the byte clock or the escape clock, plus the per-rate table.
**/
STATIC
VOID
InnoComputeTimings (
  IN     UINT64         Rate,
  IN OUT RK_DSI_CONFIG  *Config
  )
{
  STATIC CONST struct {
    UINT32    Rate;
    UINT8     HsPrepare;
    UINT8     ClkLaneHsZero;
    UINT8     DataLaneHsZero;
    UINT8     HsTrail;
  } Timings[] = {
    { 110000000,  0x20, 0x16, 0x02, 0x22 },
    { 150000000,  0x06, 0x16, 0x03, 0x45 },
    { 200000000,  0x18, 0x17, 0x04, 0x0b },
    { 250000000,  0x05, 0x17, 0x05, 0x16 },
    { 300000000,  0x51, 0x18, 0x06, 0x2c },
    { 400000000,  0x64, 0x19, 0x07, 0x33 },
    { 500000000,  0x20, 0x1b, 0x07, 0x4e },
    { 600000000,  0x6a, 0x1d, 0x08, 0x3a },
    { 700000000,  0x3e, 0x1e, 0x08, 0x6a },
    { 800000000,  0x21, 0x1f, 0x09, 0x29 },
    { 1000000000, 0x09, 0x20, 0x09, 0x27 },
  };
  UINT32  TxByteClkHs;
  UINT32  TTxByteClkHs;
  UINT32  EscClkDiv;
  UINT32  TxClkEsc;
  UINT32  TTxClkEsc;
  UINT32  Ui;
  UINT32  ClkPostPs;
  UINT32  ClkPrePs;
  UINT32  LpxPs;
  UINT32  Index;

  TxByteClkHs  = (UINT32)(Rate / 8);
  TTxByteClkHs = (UINT32)(PSEC_PER_SEC / TxByteClkHs);
  EscClkDiv    = DIV_ROUND_UP_U32 (TxByteClkHs, 20000000);
  TxClkEsc     = TxByteClkHs / EscClkDiv;
  TTxClkEsc    = (UINT32)(PSEC_PER_SEC / TxClkEsc);
  Ui           = (UINT32)(PSEC_PER_SEC / Rate);

  // mipi_dphy_timing_get_default (picoseconds)
  ClkPostPs = 70000 + 52 * Ui;
  ClkPrePs  = 8 * Ui;
  LpxPs     = 60000;

  Config->HsExit  = DIV_ROUND_UP_U32 (120000, TTxByteClkHs);
  Config->ClkPost = DIV_ROUND_UP_U32 (ClkPostPs, TTxByteClkHs);
  Config->ClkPre  = DIV_ROUND_UP_U32 (ClkPrePs, TTxByteClkHs);
  // Tlpx = Ttxbyteclkhs * (2 + value)
  Config->Lpx = DIV_ROUND_UP_U32 (LpxPs, TTxByteClkHs);
  if (Config->Lpx >= 2) {
    Config->Lpx -= 2;
  }

  Config->TaGo   = DIV_ROUND_UP_U32 (4 * LpxPs, TTxClkEsc);
  Config->TaSure = DIV_ROUND_UP_U32 (2 * LpxPs, TTxClkEsc);
  Config->TaWait = DIV_ROUND_UP_U32 (5 * LpxPs, TTxClkEsc);

  for (Index = 0; Index < sizeof (Timings) / sizeof (Timings[0]); Index++) {
    if (Rate <= Timings[Index].Rate) {
      break;
    }
  }

  if (Index == sizeof (Timings) / sizeof (Timings[0])) {
    Index--;
  }

  Config->HsPrepare  = Timings[Index].HsPrepare;
  Config->HsTrail    = Timings[Index].HsTrail;
  Config->ClkHsZero  = Timings[Index].ClkLaneHsZero;
  Config->DataHsZero = Timings[Index].DataLaneHsZero;
  Config->Wakeup     = 0x3FF;
}

/**
  dw_mipi_dsi_get_hcomponent_lbcc: a horizontal length in pixels as lane
  byte clock cycles, rounded to nearest. 32-bit like U-Boot.
**/
STATIC
UINT32
DsiLbcc (
  IN UINT32  HComponent,
  IN UINT32  LaneMbps,
  IN UINT32  ClockKhz
  )
{
  UINT32  Lbcc;

  if (ClockKhz == 0) {
    return 0;
  }

  Lbcc = HComponent * LaneMbps * 1000 / 8;
  return (Lbcc + ClockKhz / 2) / ClockKhz;
}

EFI_STATUS
RkDsiComputeConfig (
  IN  CONST RK_PANEL_DESC  *Panel,
  OUT RK_DSI_CONFIG        *Config
  )
{
  CONST RK_DISPLAY_MODE  *Mode;

  Mode                = &Panel->Mode;
  Config->LaneRateBps = RkDsiLaneRate (Mode, Panel->Format, Panel->Lanes, Panel->LaneRateMbps);
  Config->PhyRateBps  = RkInnoPllRoundRate (24000000, Config->LaneRateBps, &Config->PhyPrediv, &Config->PhyFbdiv);
  if (Config->PhyRateBps == 0) {
    return EFI_UNSUPPORTED;
  }

  Config->LaneMbps  = (UINT32)(Config->PhyRateBps / 1000 / 1000);
  Config->EscClkDiv = DIV_ROUND_UP_U32 (Config->LaneMbps >> 3, 20);
  Config->HlineLbcc = DsiLbcc (Mode->HTotal, Config->LaneMbps, Mode->Clock);
  Config->HsaLbcc   = DsiLbcc (Mode->HSyncEnd - Mode->HSyncStart, Config->LaneMbps, Mode->Clock);
  Config->HbpLbcc   = DsiLbcc (Mode->HTotal - Mode->HSyncEnd, Config->LaneMbps, Mode->Clock);
  Config->Vsa       = Mode->VSyncEnd - Mode->VSyncStart;
  Config->Vfp       = Mode->VSyncStart - Mode->VDisplay;
  Config->Vbp       = Mode->VTotal - Mode->VSyncEnd;
  InnoComputeTimings (Config->PhyRateBps, Config);
  return EFI_SUCCESS;
}

/**
  rockchip_vop_init and rockchip_vop_set_plane values for a full-screen
  window of Width x Height at BitsPerPixel.
**/
VOID
RkVopComputeConfig (
  IN  CONST RK_DISPLAY_MODE  *Mode,
  IN  UINT32                 Width,
  IN  UINT32                 Height,
  IN  UINT32                 BitsPerPixel,
  OUT RK_VOP_CONFIG          *Config
  )
{
  UINT16  HSyncLen;
  UINT16  HActSt;
  UINT16  HActEnd;
  UINT16  VSyncLen;
  UINT16  VActSt;
  UINT16  VActEnd;
  UINT16  DspStX;
  UINT16  DspStY;

  HSyncLen = (UINT16)(Mode->HSyncEnd - Mode->HSyncStart);
  HActSt   = (UINT16)(Mode->HTotal - Mode->HSyncStart);
  HActEnd  = (UINT16)(HActSt + Mode->HDisplay);
  VSyncLen = (UINT16)(Mode->VSyncEnd - Mode->VSyncStart);
  VActSt   = (UINT16)(Mode->VTotal - Mode->VSyncStart);
  VActEnd  = (UINT16)(VActSt + Mode->VDisplay);

  Config->HTotalPw  = ((UINT32)(UINT16)Mode->HTotal << 16) | HSyncLen;
  Config->HActStEnd = ((UINT32)HActSt << 16) | HActEnd;
  Config->VActStEnd = ((UINT32)VActSt << 16) | VActEnd;
  Config->VTotalPw  = ((UINT32)(UINT16)Mode->VTotal << 16) | VSyncLen;
  Config->LineFlag  = (UINT32)VActEnd - 3;

  Config->PinPol  = 0x8;
  Config->PinPol |= (Mode->Flags & RK_MODE_NHSYNC) ? 0 : 1;
  Config->PinPol |= (Mode->Flags & RK_MODE_NVSYNC) ? 0 : 2;
  Config->DclkInv = (Mode->Flags & RK_MODE_PPIXDATA) ? 0 : 1;

  // Window at (0, 0) of the active area, framebuffer pixels 1:1.
  DspStX           = (UINT16)(Mode->HTotal - Mode->HSyncStart);
  DspStY           = (UINT16)(Mode->VTotal - Mode->VSyncStart);
  Config->DspInfo  = ((Height - 1) << 16) | ((Width - 1) & 0xFFFF);
  Config->DspSt    = ((UINT32)DspStY << 16) | DspStX;
  Config->Vir      = ((Width * BitsPerPixel + 31) & ~31U) >> 5;
}

/**
  pwm_backlight_enable: duty = period * (level - min) / (max - min + 1),
  min_level always 0.
**/
UINT32
RkBacklightDutyNs (
  IN CONST RK_BACKLIGHT_DESC  *Backlight
  )
{
  return Backlight->PeriodNs * Backlight->DefaultLevel / (Backlight->MaxLevel + 1);
}

/**
  rk_pwm_set_config: counter ticks for Ns at ClockHz (prescaler 1 on v3).
**/
UINT32
RkPwmCount (
  IN UINT64  ClockHz,
  IN UINT32  Ns
  )
{
  return (UINT32)(((ClockHz / 1000) * Ns) / 1000000);
}

/**
  rockchip_panel_parse_cmds: [type, delay_ms, length, payload...] records.
  FALSE at the end or on a record that runs past the property (U-Boot does
  not check that; a cut-off record is not sent).
**/
BOOLEAN
RkPanelNextCmd (
  IN     CONST UINT8   *Seq,
  IN     UINT32        SeqLen,
  IN OUT UINT32        *Offset,
  OUT    RK_PANEL_CMD  *Cmd
  )
{
  UINT32  Left;

  if ((Seq == NULL) || (*Offset > SeqLen)) {
    return FALSE;
  }

  Left = SeqLen - *Offset;
  if (Left <= 3) {
    return FALSE;
  }

  Cmd->Type    = Seq[*Offset];
  Cmd->DelayMs = Seq[*Offset + 1];
  Cmd->Length  = Seq[*Offset + 2];
  Cmd->Payload = &Seq[*Offset + 3];
  if ((UINT32)Cmd->Length > Left - 3) {
    return FALSE;
  }

  *Offset += 3 + Cmd->Length;
  return TRUE;
}

/**
  rockchip_panel_send_dsi_cmds: generic types go out as
  mipi_dsi_generic_write, DCS types as mipi_dsi_dcs_write_buffer; both pick
  the packet type by length. 0 = not sendable.
**/
UINT8
RkPanelCmdMsgType (
  IN UINT8  CmdType,
  IN UINT8  Length
  )
{
  switch (CmdType) {
    case MIPI_DSI_GENERIC_SHORT_WRITE_0:
    case MIPI_DSI_GENERIC_SHORT_WRITE_1:
    case MIPI_DSI_GENERIC_SHORT_WRITE_2:
    case MIPI_DSI_GENERIC_LONG_WRITE:
      switch (Length) {
        case 0:
          return MIPI_DSI_GENERIC_SHORT_WRITE_0;
        case 1:
          return MIPI_DSI_GENERIC_SHORT_WRITE_1;
        case 2:
          return MIPI_DSI_GENERIC_SHORT_WRITE_2;
        default:
          return MIPI_DSI_GENERIC_LONG_WRITE;
      }

    case MIPI_DSI_DCS_SHORT_WRITE:
    case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
    case MIPI_DSI_DCS_LONG_WRITE:
      switch (Length) {
        case 0:
          return 0;
        case 1:
          return MIPI_DSI_DCS_SHORT_WRITE;
        case 2:
          return MIPI_DSI_DCS_SHORT_WRITE_PARAM;
        default:
          return MIPI_DSI_DCS_LONG_WRITE;
      }

    default:
      return 0;
  }
}

BOOLEAN
RkDsiMsgTypeIsLong (
  IN UINT8  MsgType
  )
{
  return (MsgType == MIPI_DSI_GENERIC_LONG_WRITE) || (MsgType == MIPI_DSI_DCS_LONG_WRITE);
}

/**
  mipi_dsi_create_packet: data ID, then the length (long) or the first two
  payload bytes (short). The DW host adds the ECC.
**/
UINT32
RkDsiPacketHeader (
  IN UINT8        Channel,
  IN UINT8        MsgType,
  IN CONST UINT8  *Payload,
  IN UINT32       Length
  )
{
  UINT32  Header;

  Header = ((UINT32)(Channel & 0x3) << 6) | (MsgType & 0x3F);
  if (RkDsiMsgTypeIsLong (MsgType)) {
    Header |= (Length & 0xFF) << 8;
    Header |= ((Length >> 8) & 0xFF) << 16;
  } else {
    Header |= (Length > 0) ? ((UINT32)Payload[0] << 8) : 0;
    Header |= (Length > 1) ? ((UINT32)Payload[1] << 16) : 0;
  }

  return Header;
}

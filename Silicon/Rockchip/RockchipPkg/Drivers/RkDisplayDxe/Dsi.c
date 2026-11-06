/** @file
  Synopsys DesignWare MIPI DSI host on PX30, with the external Inno PHY.

  Ported from U-Boot drivers/video/drm/dw_mipi_dsi.c: host init, D-PHY
  bring-up through the host's PHY_RSTZ/TST_CTRL bits and the GRF, video and
  command mode switches, and the generic interface packet transfer that
  sends the panel's init commands. Only the GRF fields px30_dsi_grf_reg_fields
  lists are written; the others do not exist on PX30.

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include "RkDisplay.h"

#define DSI_PWR_UP              0x04
#define DSI_CLKMGR_CFG          0x08
#define DSI_DPI_VCID            0x0C
#define DSI_DPI_COLOR_CODING    0x10
#define DSI_DPI_CFG_POL         0x14
#define DSI_DPI_LP_CMD_TIM      0x18
#define DSI_DBI_VCID            0x1C
#define DSI_PCKHDL_CFG          0x2C
#define DSI_MODE_CFG            0x34
#define DSI_VID_MODE_CFG        0x38
#define DSI_VID_PKT_SIZE        0x3C
#define DSI_VID_HSA_TIME        0x48
#define DSI_VID_HBP_TIME        0x4C
#define DSI_VID_HLINE_TIME      0x50
#define DSI_VID_VSA_LINES       0x54
#define DSI_VID_VBP_LINES       0x58
#define DSI_VID_VFP_LINES       0x5C
#define DSI_VID_VACTIVE_LINES   0x60
#define DSI_EDPI_CMD_SIZE       0x64
#define DSI_CMD_MODE_CFG        0x68
#define DSI_GEN_HDR             0x6C
#define DSI_GEN_PLD_DATA        0x70
#define DSI_CMD_PKT_STATUS      0x74
#define DSI_TO_CNT_CFG          0x78
#define DSI_BTA_TO_CNT          0x8C
#define DSI_LPCLK_CTRL          0x94
#define DSI_PHY_TMR_LPCLK_CFG   0x98
#define DSI_PHY_TMR_CFG         0x9C
#define DSI_PHY_RSTZ            0xA0
#define DSI_PHY_IF_CFG          0xA4
#define DSI_PHY_STATUS          0xB0
#define DSI_PHY_TST_CTRL0       0xB4
#define DSI_INT_ST0             0xBC
#define DSI_INT_ST1             0xC0
#define DSI_INT_MSK0            0xC4
#define DSI_INT_MSK1            0xC8

#define POWERUP                 BIT0
#define CMD_VIDEO_MODE          BIT0
#define COMMAND_MODE            BIT0
#define LP_CMD_EN               BIT15
#define LP_HFP_EN               BIT13
#define LP_HBP_EN               BIT12
#define LP_VACT_EN              BIT11
#define LP_VFP_EN               BIT10
#define LP_VBP_EN               BIT9
#define LP_VSA_EN               BIT8
#define VID_MODE_TYPE_BURST_SYNC_PULSES  0x0
#define VID_MODE_TYPE_BURST_SYNC_EVENTS  0x1
#define VID_MODE_TYPE_BURST              0x2
#define DCS_LW_TX               BIT19
#define DCS_SW_1P_TX            BIT17
#define DCS_SW_0P_TX            BIT16
#define GEN_LW_TX               BIT14
#define GEN_SW_2P_TX            BIT10
#define GEN_SW_1P_TX            BIT9
#define GEN_SW_0P_TX            BIT8
#define GEN_CMD_EMPTY           BIT0
#define GEN_CMD_FULL            BIT1
#define GEN_PLD_W_EMPTY         BIT2
#define GEN_PLD_W_FULL          BIT3
#define AUTO_CLKLANE_CTRL       BIT1
#define PHY_TXREQUESTCLKHS      BIT0
#define PHY_ENABLECLK           BIT2
#define PHY_RSTZ                BIT1
#define PHY_SHUTDOWNZ           BIT0
#define PHY_LOCK                BIT0
#define PHY_STOPSTATELANE       (BIT4 | BIT2)  // data lane 0 and clock lane
#define PHY_TESTCLR             BIT0
#define CRC_RX_EN               BIT4
#define ECC_RX_EN               BIT3
#define BTA_EN                  BIT2
#define EOTP_TX_EN              BIT0

#define PHY_STATUS_TIMEOUT_US      10000
#define CMD_PKT_STATUS_TIMEOUT_US  20000

// px30_dsi_grf_reg_fields: GRF_PD_VO_CON0/1, write-masked
#define GRF_PD_VO_CON0          0x434
#define GRF_PD_VO_CON1          0x438

STATIC
VOID
DsiWrite (
  IN UINTN   Base,
  IN UINT32  Reg,
  IN UINT32  Value
  )
{
  MmioWrite32 (Base + Reg, Value);
}

STATIC
VOID
DsiUpdate (
  IN UINTN   Base,
  IN UINT32  Reg,
  IN UINT32  Mask,
  IN UINT32  Value
  )
{
  MmioWrite32 (Base + Reg, (MmioRead32 (Base + Reg) & ~Mask) | (Value & Mask));
}

/**
  grf_field_write: GENMASK(Msb, Lsb) of a GRF register, with its write mask.
**/
STATIC
VOID
GrfField (
  IN UINT32  Reg,
  IN UINT32  Lsb,
  IN UINT32  Msb,
  IN UINT32  Value
  )
{
  UINT32  Mask;

  Mask = ((1U << (Msb - Lsb + 1)) - 1) << Lsb;
  MmioWrite32 (RK_GRF_BASE + Reg, (Mask << 16) | ((Value << Lsb) & Mask));
}

STATIC
EFI_STATUS
PollBits (
  IN UINTN    Address,
  IN UINT32   Mask,
  IN UINT32   Want,
  IN UINTN    TimeoutUs,
  OUT UINT32  *Last
  )
{
  UINTN  Waited;

  for (Waited = 0; ; Waited++) {
    *Last = MmioRead32 (Address);
    if ((*Last & Mask) == Want) {
      return EFI_SUCCESS;
    }

    if (Waited >= TimeoutUs) {
      return EFI_TIMEOUT;
    }

    MicroSecondDelay (1);
  }
}

VOID
RkDsiVopRouting (
  IN UINT32  CrtcId
  )
{
  GrfField (GRF_PD_VO_CON1, 0, 0, CrtcId);   // VOPSEL
}

STATIC
VOID
DsiHostInit (
  IN RK_DISPLAY  *Display
  )
{
  CONST RK_PANEL_DESC    *Panel;
  CONST RK_DSI_CONFIG    *Cfg;
  CONST RK_DISPLAY_MODE  *Mode;
  UINTN                  Base;
  UINT32                 Val;
  UINT32                 Color;

  Panel = &Display->Panel;
  Cfg   = &Display->Dsi;
  Mode  = &Panel->Mode;
  Base  = Panel->DsiBase;

  // dw_mipi_dsi_init: escape clock at most 20 MHz
  DsiWrite (Base, DSI_PWR_UP, 0);
  DsiWrite (Base, DSI_CLKMGR_CFG, (10 << 8) | (Cfg->EscClkDiv & 0xFF));

  // dw_mipi_dsi_dpi_config
  switch (Panel->Format) {
    case RK_DSI_FMT_RGB666:
      Color = 0x4 | BIT8;           // 18-bit config 2, loosely packed
      break;
    case RK_DSI_FMT_RGB666_PACKED:
      Color = 0x3;
      break;
    case RK_DSI_FMT_RGB565:
      Color = 0x0;
      break;
    default:
      Color = 0x5;                  // 24-bit
      break;
  }

  Val  = (Mode->Flags & RK_MODE_NVSYNC) ? BIT1 : 0;
  Val |= (Mode->Flags & RK_MODE_NHSYNC) ? BIT2 : 0;
  DsiWrite (Base, DSI_DPI_VCID, Panel->Channel & 0x3);
  DsiWrite (Base, DSI_DPI_COLOR_CODING, Color);
  DsiWrite (Base, DSI_DPI_CFG_POL, Val);
  DsiWrite (Base, DSI_DPI_LP_CMD_TIM, (4 << 16) | 4);

  // dw_mipi_dsi_packet_handler_config
  Val = CRC_RX_EN | ECC_RX_EN | BTA_EN | EOTP_TX_EN;
  if (Panel->ModeFlags & RK_DSI_MODE_EOT_PACKET) {
    Val &= ~EOTP_TX_EN;
  }

  DsiWrite (Base, DSI_PCKHDL_CFG, Val);

  // dw_mipi_dsi_video_mode_config
  Val = LP_VACT_EN | LP_VFP_EN | LP_VBP_EN | LP_VSA_EN | LP_HFP_EN | LP_HBP_EN;
  if (Panel->ModeFlags & RK_DSI_MODE_HFP) {
    Val &= ~LP_HFP_EN;
  }

  if (Panel->ModeFlags & RK_DSI_MODE_HBP) {
    Val &= ~LP_HBP_EN;
  }

  if (Panel->ModeFlags & RK_DSI_MODE_BURST) {
    Val |= VID_MODE_TYPE_BURST;
  } else if (Panel->ModeFlags & RK_DSI_MODE_SYNC_PULSE) {
    Val |= VID_MODE_TYPE_BURST_SYNC_PULSES;
  } else {
    Val |= VID_MODE_TYPE_BURST_SYNC_EVENTS;
  }

  DsiWrite (Base, DSI_VID_MODE_CFG, Val);
  if (Panel->ModeFlags & RK_DSI_CLOCK_NON_CONTINUOUS) {
    DsiUpdate (Base, DSI_LPCLK_CTRL, AUTO_CLKLANE_CTRL, AUTO_CLKLANE_CTRL);
  }

  DsiWrite (Base, DSI_VID_PKT_SIZE, Mode->HDisplay & 0x3FFF);
  DsiWrite (Base, DSI_TO_CNT_CFG, (1000 << 16) | 1000);
  DsiWrite (Base, DSI_BTA_TO_CNT, 0xD00);
  DsiUpdate (Base, DSI_MODE_CFG, CMD_VIDEO_MODE, COMMAND_MODE);

  // Line and frame timing
  DsiWrite (Base, DSI_VID_HLINE_TIME, Cfg->HlineLbcc);
  DsiWrite (Base, DSI_VID_HSA_TIME, Cfg->HsaLbcc);
  DsiWrite (Base, DSI_VID_HBP_TIME, Cfg->HbpLbcc);
  DsiWrite (Base, DSI_VID_VACTIVE_LINES, Mode->VDisplay);
  DsiWrite (Base, DSI_VID_VSA_LINES, Cfg->Vsa);
  DsiWrite (Base, DSI_VID_VFP_LINES, Cfg->Vfp);
  DsiWrite (Base, DSI_VID_VBP_LINES, Cfg->Vbp);

  // dphy timing and interface
  DsiWrite (Base, DSI_PHY_TMR_CFG, (0x14 << 24) | (0x10 << 16) | 10000);
  DsiWrite (Base, DSI_PHY_TMR_LPCLK_CFG, (0x40 << 16) | 0x40);
  DsiWrite (Base, DSI_PHY_IF_CFG, (0x20 << 8) | ((Panel->Lanes - 1) & 0x3));

  // dw_mipi_dsi_clear_err
  MmioRead32 (Base + DSI_INT_ST0);
  MmioRead32 (Base + DSI_INT_ST1);
  DsiWrite (Base, DSI_INT_MSK0, 0);
  DsiWrite (Base, DSI_INT_MSK1, 0);
}

/**
  mipi_dphy_init: hold the PHY in reset, release TESTCLR, then let the
  PHY clock run.
**/
STATIC
VOID
DsiDphyInit (
  IN UINTN  Base
  )
{
  DsiUpdate (Base, DSI_PHY_RSTZ, PHY_ENABLECLK, 0);
  MicroSecondDelay (1);
  DsiUpdate (Base, DSI_PHY_RSTZ, PHY_SHUTDOWNZ, 0);
  MicroSecondDelay (1);
  DsiUpdate (Base, DSI_PHY_RSTZ, PHY_RSTZ, 0);
  MicroSecondDelay (1);
  DsiUpdate (Base, DSI_PHY_TST_CTRL0, PHY_TESTCLR, PHY_TESTCLR);
  MicroSecondDelay (1);

  GrfField (GRF_PD_VO_CON1, 5, 5, 0);       // TURNDISABLE
  GrfField (GRF_PD_VO_CON1, 7, 10, 0);      // FORCETXSTOPMODE
  MicroSecondDelay (1);

  DsiUpdate (Base, DSI_PHY_TST_CTRL0, PHY_TESTCLR, 0);
  MicroSecondDelay (1);
  DsiUpdate (Base, DSI_PHY_RSTZ, PHY_ENABLECLK, PHY_ENABLECLK);
  MicroSecondDelay (1);
}

/**
  mipi_dphy_power_on: out of reset, PHY PLL and lanes up, wait for lock
  and for the lanes to reach stop state.
**/
STATIC
EFI_STATUS
DsiDphyPowerOn (
  IN RK_DISPLAY  *Display
  )
{
  UINTN       Base;
  UINT32      Status;
  EFI_STATUS  Result;

  Base = Display->Panel.DsiBase;
  DsiUpdate (Base, DSI_PHY_RSTZ, PHY_SHUTDOWNZ, PHY_SHUTDOWNZ);
  MicroSecondDelay (1);
  DsiUpdate (Base, DSI_PHY_RSTZ, PHY_RSTZ, PHY_RSTZ);
  MicroSecondDelay (1);
  MicroSecondDelay (2000);

  RkInnoPhyPowerOn (Display->Panel.PhyBase, &Display->Dsi);

  Result = PollBits (Base + DSI_PHY_STATUS, PHY_LOCK, PHY_LOCK, PHY_STATUS_TIMEOUT_US, &Status);
  if (EFI_ERROR (Result)) {
    DEBUG ((DEBUG_ERROR, "Dsi: PHY is not locked (PHY_STATUS 0x%08x)\n", Status));
    return Result;
  }

  MicroSecondDelay (200);
  Result = PollBits (Base + DSI_PHY_STATUS, PHY_STOPSTATELANE, PHY_STOPSTATELANE, PHY_STATUS_TIMEOUT_US, &Status);
  if (EFI_ERROR (Result)) {
    DEBUG ((DEBUG_ERROR, "Dsi: lanes not in stop state (PHY_STATUS 0x%08x)\n", Status));
    return Result;
  }

  MicroSecondDelay (10);
  DEBUG ((DEBUG_INFO, "Dsi: PHY locked, lanes in stop state (PHY_STATUS 0x%08x)\n", Status));
  return EFI_SUCCESS;
}

/**
  dw_mipi_dsi_connector_prepare after the rate is known: route the VOP,
  host init, PHY up, host powered. Like U-Boot this carries on when the PHY
  does not lock; the return value says so.
**/
EFI_STATUS
RkDsiPrepare (
  IN RK_DISPLAY  *Display
  )
{
  EFI_STATUS  Status;
  UINTN       Base;

  Base = Display->Panel.DsiBase;
  RkDsiVopRouting (Display->Panel.CrtcId);
  DsiHostInit (Display);
  DsiDphyInit (Base);
  Status = DsiDphyPowerOn (Display);
  DsiWrite (Base, DSI_PWR_UP, POWERUP);

  DEBUG ((
    DEBUG_INFO,
    "Dsi: readback CLKMGR 0x%08x VID_MODE 0x%08x PKT 0x%08x HLINE %u HSA %u HBP %u VSA %u VBP %u VFP %u VACT %u PHY_IF 0x%08x\n",
    MmioRead32 (Base + DSI_CLKMGR_CFG),
    MmioRead32 (Base + DSI_VID_MODE_CFG),
    MmioRead32 (Base + DSI_VID_PKT_SIZE),
    MmioRead32 (Base + DSI_VID_HLINE_TIME),
    MmioRead32 (Base + DSI_VID_HSA_TIME),
    MmioRead32 (Base + DSI_VID_HBP_TIME),
    MmioRead32 (Base + DSI_VID_VSA_LINES),
    MmioRead32 (Base + DSI_VID_VBP_LINES),
    MmioRead32 (Base + DSI_VID_VFP_LINES),
    MmioRead32 (Base + DSI_VID_VACTIVE_LINES),
    MmioRead32 (Base + DSI_PHY_IF_CFG)
    ));
  DEBUG ((
    DEBUG_INFO,
    "Dsi: readback GRF PD_VO_CON0 0x%08x CON1 0x%08x\n",
    MmioRead32 (RK_GRF_BASE + GRF_PD_VO_CON0),
    MmioRead32 (RK_GRF_BASE + GRF_PD_VO_CON1)
    ));
  return Status;
}

VOID
RkDsiEnable (
  IN RK_DISPLAY  *Display
  )
{
  UINTN  Base;

  Base = Display->Panel.DsiBase;
  DsiUpdate (Base, DSI_LPCLK_CTRL, PHY_TXREQUESTCLKHS, PHY_TXREQUESTCLKHS);
  DsiWrite (Base, DSI_PWR_UP, 0);
  if (Display->Panel.ModeFlags & RK_DSI_MODE_VIDEO) {
    DsiUpdate (Base, DSI_MODE_CFG, CMD_VIDEO_MODE, 0);
  } else {
    DsiWrite (Base, DSI_DBI_VCID, Display->Panel.Channel & 0x3);
    DsiUpdate (Base, DSI_CMD_MODE_CFG, DCS_LW_TX, 0);
    DsiWrite (Base, DSI_EDPI_CMD_SIZE, Display->Panel.Mode.HDisplay);
    DsiUpdate (Base, DSI_MODE_CFG, CMD_VIDEO_MODE, COMMAND_MODE);
  }

  DsiWrite (Base, DSI_PWR_UP, POWERUP);
  DEBUG ((
    DEBUG_INFO,
    "Dsi: %a mode, MODE_CFG 0x%08x LPCLK 0x%08x\n",
    (Display->Panel.ModeFlags & RK_DSI_MODE_VIDEO) ? "video" : "command",
    MmioRead32 (Base + DSI_MODE_CFG),
    MmioRead32 (Base + DSI_LPCLK_CTRL)
    ));
}

VOID
RkDsiDisable (
  IN UINTN  DsiBase
  )
{
  DsiWrite (DsiBase, DSI_PWR_UP, 0);
  DsiWrite (DsiBase, DSI_LPCLK_CTRL, 0);
  DsiWrite (DsiBase, DSI_EDPI_CMD_SIZE, 0);
  DsiUpdate (DsiBase, DSI_MODE_CFG, CMD_VIDEO_MODE, COMMAND_MODE);
  DsiWrite (DsiBase, DSI_PWR_UP, POWERUP);
}

VOID
RkDsiPostDisable (
  IN UINTN  DsiBase
  )
{
  DsiWrite (DsiBase, DSI_PWR_UP, 0);
  DsiWrite (DsiBase, DSI_PHY_RSTZ, 0);
}

/**
  dw_mipi_dsi_transfer for the write types panel sequences use: pick LP or
  HS for the command, allow the packet type in LP when the panel wants LPM,
  push the payload in 32-bit words, then the header, and wait for the FIFOs
  to drain.
**/
EFI_STATUS
RkDsiTransfer (
  IN UINTN        DsiBase,
  IN UINT32       ModeFlags,
  IN UINT8        Channel,
  IN UINT8        MsgType,
  IN CONST UINT8  *Payload,
  IN UINT32       Length
  )
{
  UINT32       TypeBit;
  BOOLEAN      Lpm;
  UINT32       Status;
  UINT32       Word;
  UINT32       Left;
  CONST UINT8  *Data;
  EFI_STATUS   Result;

  Lpm = (ModeFlags & RK_DSI_MODE_LPM) != 0;
  if (Lpm) {
    DsiUpdate (DsiBase, DSI_VID_MODE_CFG, LP_CMD_EN, LP_CMD_EN);
    DsiUpdate (DsiBase, DSI_LPCLK_CTRL, PHY_TXREQUESTCLKHS, 0);
  } else {
    DsiUpdate (DsiBase, DSI_VID_MODE_CFG, LP_CMD_EN, 0);
    DsiUpdate (DsiBase, DSI_LPCLK_CTRL, PHY_TXREQUESTCLKHS, PHY_TXREQUESTCLKHS);
  }

  switch (MsgType) {
    case MIPI_DSI_DCS_SHORT_WRITE:
      TypeBit = DCS_SW_0P_TX;
      break;
    case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
      TypeBit = DCS_SW_1P_TX;
      break;
    case MIPI_DSI_DCS_LONG_WRITE:
      TypeBit = DCS_LW_TX;
      break;
    case MIPI_DSI_GENERIC_SHORT_WRITE_0:
      TypeBit = GEN_SW_0P_TX;
      break;
    case MIPI_DSI_GENERIC_SHORT_WRITE_1:
      TypeBit = GEN_SW_1P_TX;
      break;
    case MIPI_DSI_GENERIC_SHORT_WRITE_2:
      TypeBit = GEN_SW_2P_TX;
      break;
    case MIPI_DSI_GENERIC_LONG_WRITE:
      TypeBit = GEN_LW_TX;
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  DsiUpdate (DsiBase, DSI_CMD_MODE_CFG, TypeBit, Lpm ? TypeBit : 0);

  if (RkDsiMsgTypeIsLong (MsgType)) {
    Data = Payload;
    Left = Length;
    while (Left > 0) {
      Result = PollBits (DsiBase + DSI_CMD_PKT_STATUS, GEN_PLD_W_FULL, 0, CMD_PKT_STATUS_TIMEOUT_US, &Status);
      if (EFI_ERROR (Result)) {
        DEBUG ((DEBUG_ERROR, "Dsi: payload FIFO full (0x%08x)\n", Status));
        return Result;
      }

      Word = 0;
      CopyMem (&Word, Data, Left < 4 ? Left : 4);   // little-endian words
      DsiWrite (DsiBase, DSI_GEN_PLD_DATA, Word);
      Data += (Left < 4) ? Left : 4;
      Left -= (Left < 4) ? Left : 4;
    }
  }

  Result = PollBits (DsiBase + DSI_CMD_PKT_STATUS, GEN_CMD_FULL, 0, CMD_PKT_STATUS_TIMEOUT_US, &Status);
  if (EFI_ERROR (Result)) {
    DEBUG ((DEBUG_ERROR, "Dsi: command FIFO full (0x%08x)\n", Status));
    return Result;
  }

  DsiWrite (DsiBase, DSI_GEN_HDR, RkDsiPacketHeader (Channel, MsgType, Payload, Length));

  Result = PollBits (
             DsiBase + DSI_CMD_PKT_STATUS,
             GEN_CMD_EMPTY | GEN_PLD_W_EMPTY,
             GEN_CMD_EMPTY | GEN_PLD_W_EMPTY,
             CMD_PKT_STATUS_TIMEOUT_US,
             &Status
             );
  if (EFI_ERROR (Result)) {
    DEBUG ((DEBUG_ERROR, "Dsi: write FIFO did not drain (0x%08x)\n", Status));
  }

  return Result;
}

/** @file
  PX30 VOPB (VOP lite, version 2.6), window 1 only.

  Ported from U-Boot drivers/video/drm/rockchip_vop.c (rockchip_vop_init,
  rockchip_vop_set_plane, rockchip_vop_enable, rockchip_vop_disable) with
  the px30_vop_big / rk3366_win1_data field table of rockchip_vop_reg.c.
  Fields that table leaves out for PX30 (gating, AXI, dclk_pol, pin_pol,
  mirroring, post scaler, line_flag[1], act_info, alpha) are not written,
  as in U-Boot.

  Like U-Boot, every write is a read-modify-write of a copy of the register
  file taken at init: these registers are shadowed until cfg_done, and the
  copy is what the next frame will see.

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include "RkDisplay.h"

#define VOP_REG_CFG_DONE      0x000
#define VOP_VERSION           0x004
#define VOP_DSP_BG            0x008
#define VOP_SYS_CTRL2         0x018
#define VOP_DSP_CTRL0         0x020
#define VOP_DSP_CTRL2         0x028
#define VOP_LINE_FLAG         0x030
#define VOP_WIN1_CTRL0        0x090
#define VOP_WIN1_VIR          0x098
#define VOP_WIN1_MST          0x0A0
#define VOP_WIN1_DSP_INFO     0x0A4
#define VOP_WIN1_DSP_ST       0x0A8
#define VOP_HTOTAL_HS_END     0x100
#define VOP_HACT_ST_END       0x104
#define VOP_VTOTAL_VS_END     0x108
#define VOP_VACT_ST_END       0x10C
#define VOP_BCSH_CTRL         0x160

#define CSC_BT709L            1       // to_vop_csc_mode(V4L2_COLORSPACE_DEFAULT)
#define ROCKCHIP_FMT_ARGB8888 0
#define ROCKCHIP_FMT_RGB888   1
#define ROCKCHIP_FMT_RGB565   2

STATIC
VOID
VopSet (
  IN OUT RK_DISPLAY  *Display,
  IN     UINT32      Offset,
  IN     UINT32      Mask,
  IN     UINT32      Shift,
  IN     UINT32      Value
  )
{
  UINT32  Reg;

  Reg                            = Display->VopRegs[Offset / 4];
  Reg                            = (Reg & ~(Mask << Shift)) | ((Value & Mask) << Shift);
  Display->VopRegs[Offset / 4]   = Reg;
  MmioWrite32 (Display->Panel.VopBase + Offset, Reg);
}

STATIC
VOID
VopCfgDone (
  IN OUT RK_DISPLAY  *Display
  )
{
  VopSet (Display, VOP_REG_CFG_DONE, 0x1, 0, 1);
}

/**
  dither_down for the panel's bus format, output P888 (DSI).
**/
STATIC
UINT32
VopDitherDown (
  IN UINT32  BusFormat
  )
{
  UINT32  Val;

  switch (BusFormat) {
    case 0x1017:                        // RGB565_1X16
      Val = BIT1;                       // DITHER_DOWN_EN, mode RGB888_TO_RGB565
      break;
    case 0x1009:                        // RGB666_1X18
    case 0x1015:                        // RGB666_1X24_CPADHI
    case 0x1010:                        // RGB666_1X7X3_SPWG
    case 0x101B:                        // RGB666_1X7X3_JEIDA
      Val = BIT1 | BIT2;                // DITHER_DOWN_EN, mode RGB888_TO_RGB666
      break;
    default:
      Val = 0;
      break;
  }

  return Val | BIT0;                    // PRE_DITHER_DOWN_EN for P888
}

VOID
RkVopInit (
  IN OUT RK_DISPLAY  *Display
  )
{
  CONST RK_VOP_CONFIG  *Cfg;
  UINT32               Index;

  Cfg = &Display->Vop;

  for (Index = 0; Index < ARRAY_SIZE (Display->VopRegs); Index++) {
    Display->VopRegs[Index] = MmioRead32 (Display->Panel.VopBase + Index * 4);
  }

  VopSet (Display, VOP_SYS_CTRL2, 0x1, 13, 1);           // global_regdone_en
  VopSet (Display, VOP_DSP_CTRL2, 0x1, 14, 0);           // dsp_blank
  VopSet (Display, VOP_DSP_CTRL0, 0x1, 24, 1);           // mipi_en
  VopSet (Display, VOP_DSP_CTRL0, 0x7, 26, Cfg->PinPol); // mipi_pin_pol
  VopSet (Display, VOP_DSP_CTRL0, 0x1, 25, Cfg->DclkInv);// mipi_dclk_pol
  VopSet (Display, VOP_DSP_CTRL2, 0x7, 6, VopDitherDown (Display->Panel.BusFormat));
  VopSet (Display, VOP_DSP_CTRL0, 0x1, 14, 0);           // dclk_ddr
  VopSet (Display, VOP_DSP_CTRL2, 0x1F, 9, 0);           // dsp_data_swap
  VopSet (Display, VOP_DSP_CTRL2, 0xF, 16, 0);           // out_mode P888
  VopSet (Display, VOP_DSP_CTRL2, 0x1, 4, 0);            // overlay_mode RGB
  VopSet (Display, VOP_BCSH_CTRL, 0x1, 7, 0);            // bcsh_r2y_en
  VopSet (Display, VOP_BCSH_CTRL, 0x1, 6, 0);            // bcsh_y2r_en
  VopSet (Display, VOP_BCSH_CTRL, 0x1, 1, CSC_BT709L);   // bcsh_r2y_csc_mode
  VopSet (Display, VOP_BCSH_CTRL, 0x3, 4, CSC_BT709L);   // bcsh_y2r_csc_mode
  VopSet (Display, VOP_DSP_BG, 0x00FFFFFF, 0, 0);        // dsp_background
  VopSet (Display, VOP_HTOTAL_HS_END, 0x0FFF0FFF, 0, Cfg->HTotalPw);
  VopSet (Display, VOP_HACT_ST_END, 0x0FFF0FFF, 0, Cfg->HActStEnd);
  VopSet (Display, VOP_VACT_ST_END, 0x0FFF0FFF, 0, Cfg->VActStEnd);
  VopSet (Display, VOP_DSP_CTRL2, 0x1, 0, 0);            // dsp_interlace
  VopSet (Display, VOP_VTOTAL_VS_END, 0x0FFF0FFF, 0, Cfg->VTotalPw);
  VopSet (Display, VOP_DSP_CTRL0, 0x1, 13, 0);           // core_dclk_div
  VopSet (Display, VOP_LINE_FLAG, 0xFFF, 0, Cfg->LineFlag);
  VopCfgDone (Display);

  if ((Display->Panel.Mode.Flags & (RK_MODE_XMIRROR | RK_MODE_YMIRROR)) != 0) {
    DEBUG ((DEBUG_WARN, "Vop: screen-rotate asks for mirroring, which VOPB window 1 cannot do (U-Boot ignores it too)\n"));
  }
}

VOID
RkVopSetPlane (
  IN OUT RK_DISPLAY  *Display
  )
{
  UINT32  Format;
  UINT32  RbSwap;

  switch (Display->FbBpp) {
    case 16:
      Format = ROCKCHIP_FMT_RGB565;
      break;
    case 24:
      Format = ROCKCHIP_FMT_RGB888;
      break;
    default:
      Format = ROCKCHIP_FMT_ARGB8888;
      break;
  }

  // display_logo: rb_swap for everything but 32 bpp; VOP 2.6 reads RGB888
  // as BGR888, so U-Boot flips it back for that format.
  RbSwap = (Display->FbBpp != 32) ? 1 : 0;
  if (Format == ROCKCHIP_FMT_RGB888) {
    RbSwap = !RbSwap;
  }

  VopSet (Display, VOP_WIN1_CTRL0, 0x7, 4, Format);
  VopSet (Display, VOP_WIN1_VIR, 0x1FFF, 0, Display->Vop.Vir);
  VopSet (Display, VOP_WIN1_MST, 0xFFFFFFFF, 0, (UINT32)Display->FbBase);
  VopSet (Display, VOP_WIN1_DSP_INFO, 0xFFFFFFFF, 0, Display->Vop.DspInfo);
  VopSet (Display, VOP_WIN1_DSP_ST, 0xFFFFFFFF, 0, Display->Vop.DspSt);
  VopSet (Display, VOP_WIN1_CTRL0, 0x1, 12, RbSwap);
  VopSet (Display, VOP_WIN1_CTRL0, 0x1, 0, 1);           // enable
  VopCfgDone (Display);
}

VOID
RkVopEnable (
  IN OUT RK_DISPLAY  *Display
  )
{
  VopSet (Display, VOP_SYS_CTRL2, 0x1, 1, 0);            // standby off
  VopCfgDone (Display);
}

/**
  rockchip_vop_disable on whatever state the VOP is in (no cached copy).
  Standby takes effect at the end of the frame being scanned out.
**/
VOID
RkVopStandby (
  IN UINTN    VopBase,
  IN BOOLEAN  WaitFrame
  )
{
  MmioWrite32 (VopBase + VOP_SYS_CTRL2, MmioRead32 (VopBase + VOP_SYS_CTRL2) | BIT1);
  MmioWrite32 (VopBase + VOP_REG_CFG_DONE, MmioRead32 (VopBase + VOP_REG_CFG_DONE) | BIT0);
  if (WaitFrame) {
    // Longest frame of any supported panel is well under 50 ms.
    MicroSecondDelay (50 * 1000);
  }
}

VOID
RkVopLogRegs (
  IN UINTN  VopBase
  )
{
  DEBUG ((
    DEBUG_INFO,
    "Vop: readback VERSION 0x%08x SYS_CTRL2 0x%08x DSP_CTRL0 0x%08x DSP_CTRL2 0x%08x LINE_FLAG 0x%08x BCSH 0x%08x\n",
    MmioRead32 (VopBase + VOP_VERSION),
    MmioRead32 (VopBase + VOP_SYS_CTRL2),
    MmioRead32 (VopBase + VOP_DSP_CTRL0),
    MmioRead32 (VopBase + VOP_DSP_CTRL2),
    MmioRead32 (VopBase + VOP_LINE_FLAG),
    MmioRead32 (VopBase + VOP_BCSH_CTRL)
    ));
  DEBUG ((
    DEBUG_INFO,
    "Vop: readback WIN1 CTRL0 0x%08x VIR 0x%08x MST 0x%08x DSP_INFO 0x%08x DSP_ST 0x%08x\n",
    MmioRead32 (VopBase + VOP_WIN1_CTRL0),
    MmioRead32 (VopBase + VOP_WIN1_VIR),
    MmioRead32 (VopBase + VOP_WIN1_MST),
    MmioRead32 (VopBase + VOP_WIN1_DSP_INFO),
    MmioRead32 (VopBase + VOP_WIN1_DSP_ST)
    ));
  DEBUG ((
    DEBUG_INFO,
    "Vop: readback HTOTAL 0x%08x HACT 0x%08x VTOTAL 0x%08x VACT 0x%08x\n",
    MmioRead32 (VopBase + VOP_HTOTAL_HS_END),
    MmioRead32 (VopBase + VOP_HACT_ST_END),
    MmioRead32 (VopBase + VOP_VTOTAL_VS_END),
    MmioRead32 (VopBase + VOP_VACT_ST_END)
    ));
}

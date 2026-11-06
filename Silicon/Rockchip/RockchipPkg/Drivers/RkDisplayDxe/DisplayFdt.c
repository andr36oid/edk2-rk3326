/** @file
  Read the panel, its DSI host, rails and backlight from the screen FDT.

  Follows what U-Boot's Rockchip DRM driver reads and how it defaults:
  rockchip_display.c (routes, get_crtc_id, display_get_timing_from_dts),
  dw_mipi_dsi.c (dw_mipi_dsi_child_post_bind, rockchip,lane-rate),
  rockchip_panel.c (rockchip_panel_ofdata_to_platdata, probe),
  pwm_backlight.c (pwm_backlight_ofdata_to_platdata),
  power/regulator/fixed.c (regulator-fixed).

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/GpioLib.h>
#include "RkDisplay.h"

#define DEFAULT_DSI_FLAGS  (RK_DSI_MODE_VIDEO | RK_DSI_MODE_BURST | RK_DSI_MODE_HBP | \
                            RK_DSI_MODE_LPM | RK_DSI_MODE_EOT_PACKET)

// px30-cru.h clock IDs of the two PWM blocks
#define SCLK_PWM0  34
#define SCLK_PWM1  35

STATIC
BOOLEAN
StrListHas (
  IN CONST CHAR8  *List,
  IN INT32        Len,
  IN CONST CHAR8  *Str
  )
{
  INT32  Pos;
  INT32  Index;

  Pos = 0;
  while ((List != NULL) && (Pos < Len)) {
    for (Index = 0; (Str[Index] != '\0') && (Pos + Index < Len) && (List[Pos + Index] == Str[Index]); Index++) {
    }

    if ((Str[Index] == '\0') && (Pos + Index < Len) && (List[Pos + Index] == '\0')) {
      return TRUE;
    }

    while ((Pos < Len) && (List[Pos] != '\0')) {
      Pos++;
    }

    Pos++;
  }

  return FALSE;
}

STATIC
BOOLEAN
NodeIsCompatible (
  IN CONST VOID   *Fdt,
  IN INT32        Node,
  IN CONST CHAR8  *Compatible
  )
{
  CONST CHAR8  *List;
  INT32        Len;

  List = FdtGetProp (Fdt, Node, "compatible", &Len);
  return StrListHas (List, Len, Compatible);
}

STATIC
BOOLEAN
NodeIsOkay (
  IN CONST VOID  *Fdt,
  IN INT32       Node
  )
{
  CONST CHAR8  *Status;
  INT32        Len;

  Status = FdtGetProp (Fdt, Node, "status", &Len);
  if (Status == NULL) {
    return TRUE;
  }

  return StrListHas (Status, Len, "okay") || StrListHas (Status, Len, "ok");
}

STATIC
BOOLEAN
GetU32 (
  IN  CONST VOID   *Fdt,
  IN  INT32        Node,
  IN  CONST CHAR8  *Name,
  OUT UINT32       *Value
  )
{
  CONST UINT32  *Prop;
  INT32         Len;

  Prop = FdtGetProp (Fdt, Node, Name, &Len);
  if ((Prop == NULL) || (Len < 4)) {
    return FALSE;
  }

  *Value = Fdt32ToCpu (Prop[0]);
  return TRUE;
}

STATIC
UINT32
GetU32Default (
  IN CONST VOID   *Fdt,
  IN INT32        Node,
  IN CONST CHAR8  *Name,
  IN UINT32       Default
  )
{
  UINT32  Value;

  return GetU32 (Fdt, Node, Name, &Value) ? Value : Default;
}

STATIC
INT32
GetPhandleNode (
  IN CONST VOID   *Fdt,
  IN INT32        Node,
  IN CONST CHAR8  *Name
  )
{
  UINT32  Phandle;

  if (!GetU32 (Fdt, Node, Name, &Phandle)) {
    return -1;
  }

  return FdtNodeOffsetByPhandle (Fdt, Phandle);
}

/**
  First address of "reg", with the parent's #address-cells.
**/
STATIC
UINTN
GetRegBase (
  IN CONST VOID  *Fdt,
  IN INT32       Node
  )
{
  CONST UINT32  *Reg;
  INT32         Len;
  INT32         Cells;

  Reg = FdtGetProp (Fdt, Node, "reg", &Len);
  if (Reg == NULL) {
    return 0;
  }

  Cells = FdtAddressCells (Fdt, FdtParentOffset (Fdt, Node));
  if ((Cells == 2) && (Len >= 8)) {
    return (UINTN)(((UINT64)Fdt32ToCpu (Reg[0]) << 32) | Fdt32ToCpu (Reg[1]));
  }

  return (Len >= 4) ? (UINTN)Fdt32ToCpu (Reg[0]) : 0;
}

/**
  <&gpioN pin flags> at Index of Name. GPIO banks are told apart by address.
**/
STATIC
VOID
GetGpio (
  IN  CONST VOID    *Fdt,
  IN  INT32         Node,
  IN  CONST CHAR8   *Name,
  OUT RK_GPIO_SPEC  *Gpio
  )
{
  CONST UINT32  *Prop;
  INT32         Len;
  INT32         Ctrl;
  UINTN         Base;
  UINT8         Bank;

  ZeroMem (Gpio, sizeof (*Gpio));
  Prop = FdtGetProp (Fdt, Node, Name, &Len);
  if ((Prop == NULL) || (Len < 12)) {
    return;
  }

  Ctrl = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Prop[0]));
  if (Ctrl < 0) {
    DEBUG ((DEBUG_WARN, "Display: %a: no GPIO controller\n", Name));
    return;
  }

  Base = GetRegBase (Fdt, Ctrl);
  for (Bank = 0; Bank < 4; Bank++) {
    if (Base == GPIO_BASE (Bank)) {
      break;
    }
  }

  if ((Bank == 4) || (Fdt32ToCpu (Prop[1]) > 31)) {
    DEBUG ((DEBUG_WARN, "Display: %a: unknown GPIO 0x%lx/%u\n", Name, (UINT64)Base, Fdt32ToCpu (Prop[1])));
    return;
  }

  Gpio->Valid     = TRUE;
  Gpio->Bank      = Bank;
  Gpio->Pin       = (UINT8)Fdt32ToCpu (Prop[1]);
  Gpio->ActiveLow = (Fdt32ToCpu (Prop[2]) & 1) != 0;
}

/**
  A supply phandle: an RK817 regulator (the PMIC library switches it by
  node), or a regulator-fixed switched by a GPIO as U-Boot's fixed.c does
  it (the "gpio" specifier gives the polarity).
**/
STATIC
VOID
GetSupply (
  IN  CONST VOID   *Fdt,
  IN  INT32        Node,
  IN  CONST CHAR8  *Name,
  OUT RK_SUPPLY    *Supply
  )
{
  INT32  Reg;
  INT32  Len;

  ZeroMem (Supply, sizeof (*Supply));
  Reg = GetPhandleNode (Fdt, Node, Name);
  if (Reg < 0) {
    return;
  }

  Supply->Present       = TRUE;
  Supply->Node          = Reg;
  Supply->Name          = FdtGetProp (Fdt, Reg, "regulator-name", &Len);
  Supply->MinMicroVolts = GetU32Default (Fdt, Reg, "regulator-min-microvolt", 0);
  if (Supply->Name == NULL) {
    Supply->Name = FdtGetName (Fdt, Reg, NULL);
  }

  if (NodeIsCompatible (Fdt, Reg, "regulator-fixed")) {
    Supply->Fixed          = TRUE;
    Supply->StartupDelayUs = GetU32Default (Fdt, Reg, "startup-delay-us", 0);
    GetGpio (Fdt, Reg, "gpio", &Supply->Gpio);
    Supply->EnableActiveHigh = FdtGetProp (Fdt, Reg, "enable-active-high", &Len) != NULL;
    if (Supply->Gpio.Valid && !Supply->EnableActiveHigh) {
      // Linux would treat this GPIO as active low; U-Boot follows the flags.
      DEBUG ((DEBUG_WARN, "Display: %a has no enable-active-high, using the GPIO flags like U-Boot\n", Supply->Name));
    }
  }
}

/**
  The pins of the pinctrl state named State (U-Boot's rk_pwm selects
  "active" when it enables the PWM).
**/
STATIC
VOID
GetPinctrl (
  IN     CONST VOID         *Fdt,
  IN     INT32              Node,
  IN     CONST CHAR8        *State,
  IN OUT RK_BACKLIGHT_DESC  *Bl
  )
{
  CONST CHAR8   *Names;
  INT32         NamesLen;
  INT32         Pos;
  UINT32        Index;
  CHAR8         PropName[] = "pinctrl-0";
  CONST UINT32  *Groups;
  INT32         GroupsLen;
  INT32         Group;
  INT32         Cfg;
  CONST UINT32  *Pins;
  INT32         PinsLen;
  INT32         Pin;
  INT32         Len;
  RK_PINMUX     *Mux;

  Names = FdtGetProp (Fdt, Node, "pinctrl-names", &NamesLen);
  if (Names == NULL) {
    return;
  }

  // Index of State in pinctrl-names
  for (Pos = 0, Index = 0; Pos < NamesLen; Index++) {
    if (AsciiStrCmp (&Names[Pos], State) == 0) {
      break;
    }

    while ((Pos < NamesLen) && (Names[Pos] != '\0')) {
      Pos++;
    }

    Pos++;
  }

  if ((Pos >= NamesLen) || (Index > 9)) {
    return;
  }

  PropName[8] = (CHAR8)('0' + Index);
  Groups      = FdtGetProp (Fdt, Node, PropName, &GroupsLen);
  for (Group = 0; (Groups != NULL) && (Group < GroupsLen / 4); Group++) {
    Cfg  = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Groups[Group]));
    Pins = (Cfg < 0) ? NULL : FdtGetProp (Fdt, Cfg, "rockchip,pins", &PinsLen);
    for (Pin = 0; (Pins != NULL) && (Pin + 4 <= PinsLen / 4); Pin += 4) {
      INT32  PinCfg;

      if (Bl->PinCount >= RK_MAX_PINS) {
        return;
      }

      Mux           = &Bl->Pins[Bl->PinCount++];
      Mux->Bank     = (UINT8)Fdt32ToCpu (Pins[Pin]);
      Mux->Pin      = (UINT8)Fdt32ToCpu (Pins[Pin + 1]);
      Mux->Function = (UINT8)Fdt32ToCpu (Pins[Pin + 2]);
      Mux->Pull     = -1;
      PinCfg        = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Pins[Pin + 3]));
      if (PinCfg >= 0) {
        if (FdtGetProp (Fdt, PinCfg, "bias-disable", &Len) != NULL) {
          Mux->Pull = GPIO_PIN_PULL_NONE;
        } else if (FdtGetProp (Fdt, PinCfg, "bias-pull-up", &Len) != NULL) {
          Mux->Pull = GPIO_PIN_PULL_UP;
        } else if (FdtGetProp (Fdt, PinCfg, "bias-pull-down", &Len) != NULL) {
          Mux->Pull = GPIO_PIN_PULL_DOWN;
        }
      }
    }
  }
}

STATIC
VOID
GetBacklight (
  IN  CONST VOID         *Fdt,
  IN  INT32              Panel,
  OUT RK_BACKLIGHT_DESC  *Bl
  )
{
  INT32         Node;
  INT32         Pwm;
  CONST UINT32  *Prop;
  INT32         Len;
  UINT32        Count;
  UINT32        Index;
  UINT32        ClockId;

  ZeroMem (Bl, sizeof (*Bl));
  Node = GetPhandleNode (Fdt, Panel, "backlight");
  if ((Node < 0) || !NodeIsCompatible (Fdt, Node, "pwm-backlight")) {
    return;
  }

  Prop = FdtGetProp (Fdt, Node, "pwms", &Len);
  if ((Prop == NULL) || (Len < 12)) {
    DEBUG ((DEBUG_WARN, "Display: backlight has no usable pwms\n"));
    return;
  }

  Pwm = FdtNodeOffsetByPhandle (Fdt, Fdt32ToCpu (Prop[0]));
  if ((Pwm < 0) || !NodeIsCompatible (Fdt, Pwm, "rockchip,rk3328-pwm")) {
    DEBUG ((DEBUG_WARN, "Display: backlight PWM is not a rockchip,rk3328-pwm\n"));
    return;
  }

  Bl->Present  = TRUE;
  Bl->PwmBase  = GetRegBase (Fdt, Pwm);
  Bl->Channel  = Fdt32ToCpu (Prop[1]);
  Bl->PeriodNs = Fdt32ToCpu (Prop[2]);
  Bl->Inverted = (Len >= 16) ? (Fdt32ToCpu (Prop[3]) != 0) : FALSE;

  // Which PWM block's clocks: from "clocks", else from the address.
  Prop = FdtGetProp (Fdt, Pwm, "clocks", &Len);
  ClockId = ((Prop != NULL) && (Len >= 8)) ? Fdt32ToCpu (Prop[1]) : 0;
  if (ClockId == SCLK_PWM0) {
    Bl->PwmBlock = 0;
  } else if (ClockId == SCLK_PWM1) {
    Bl->PwmBlock = 1;
  } else {
    Bl->PwmBlock = (Bl->PwmBase >= 0xFF208000UL) ? 1 : 0;
  }

  if (!NodeIsOkay (Fdt, Pwm)) {
    DEBUG ((DEBUG_WARN, "Display: backlight PWM node is disabled, using it anyway\n"));
  }

  Index = GetU32Default (Fdt, Node, "default-brightness-level", 255);
  Prop  = FdtGetProp (Fdt, Node, "brightness-levels", &Len);
  Count = (Prop != NULL) ? (UINT32)Len / 4 : 0;
  if (Count > Index) {
    Bl->DefaultLevel = Fdt32ToCpu (Prop[Index]);
    Bl->MaxLevel     = Fdt32ToCpu (Prop[Count - 1]);
    // Some Rockchip DTs list the levels high to low.
    if (Fdt32ToCpu (Prop[0]) > Bl->MaxLevel) {
      Bl->MaxLevel = Fdt32ToCpu (Prop[0]);
    }
  } else {
    Bl->DefaultLevel = Index;
    Bl->MaxLevel     = 255;
  }

  GetGpio (Fdt, Node, "enable-gpios", &Bl->Enable);
  GetSupply (Fdt, Node, "power-supply", &Bl->Supply);
  GetPinctrl (Fdt, Pwm, "active", Bl);
}

/**
  display_get_timing_from_dts: a "timing" subnode, else native-mode. A
  native-mode that points at something without hactive (seen in one
  profile's DTB) falls back to the first entry that has one.
**/
STATIC
EFI_STATUS
GetTiming (
  IN  CONST VOID     *Fdt,
  IN  INT32          Panel,
  OUT RK_PANEL_DESC  *Desc
  )
{
  INT32         Timings;
  INT32         Node;
  INT32         Child;
  UINT32        Value;
  RK_DT_TIMING  *T;

  STATIC CONST struct {
    CONST CHAR8    *Name;
    UINTN          Offset;
  } Fields[] = {
    { "hactive",         OFFSET_OF (RK_DT_TIMING, HActive)        },
    { "vactive",         OFFSET_OF (RK_DT_TIMING, VActive)        },
    { "clock-frequency", OFFSET_OF (RK_DT_TIMING, ClockFrequency) },
    { "hsync-len",       OFFSET_OF (RK_DT_TIMING, HSyncLen)       },
    { "hfront-porch",    OFFSET_OF (RK_DT_TIMING, HFrontPorch)    },
    { "hback-porch",     OFFSET_OF (RK_DT_TIMING, HBackPorch)     },
    { "vsync-len",       OFFSET_OF (RK_DT_TIMING, VSyncLen)       },
    { "vfront-porch",    OFFSET_OF (RK_DT_TIMING, VFrontPorch)    },
    { "vback-porch",     OFFSET_OF (RK_DT_TIMING, VBackPorch)     },
    { "hsync-active",    OFFSET_OF (RK_DT_TIMING, HSyncActive)    },
    { "vsync-active",    OFFSET_OF (RK_DT_TIMING, VSyncActive)    },
    { "pixelclk-active", OFFSET_OF (RK_DT_TIMING, PixelClkActive) },
  };
  UINTN  Index;

  Timings = FdtSubnodeOffset (Fdt, Panel, "display-timings");
  if (Timings < 0) {
    DEBUG ((DEBUG_ERROR, "Display: panel has no display-timings\n"));
    return EFI_NOT_FOUND;
  }

  Desc->TimingCount = 0;
  for (Child = FdtFirstSubnode (Fdt, Timings); Child >= 0; Child = FdtNextSubnode (Fdt, Child)) {
    Desc->TimingCount++;
  }

  Node = FdtSubnodeOffset (Fdt, Timings, "timing");
  if (Node < 0) {
    Node = GetPhandleNode (Fdt, Timings, "native-mode");
  }

  if ((Node < 0) || !GetU32 (Fdt, Node, "hactive", &Value)) {
    for (Child = FdtFirstSubnode (Fdt, Timings); Child >= 0; Child = FdtNextSubnode (Fdt, Child)) {
      if (GetU32 (Fdt, Child, "hactive", &Value)) {
        break;
      }
    }

    DEBUG ((
      DEBUG_WARN,
      "Display: native-mode does not point at a timing (U-Boot fails here); using %a\n",
      Child >= 0 ? FdtGetName (Fdt, Child, NULL) : "nothing"
      ));
    Node = Child;
    if (Node < 0) {
      return EFI_NOT_FOUND;
    }
  }

  T = &Desc->Timing;
  for (Index = 0; Index < sizeof (Fields) / sizeof (Fields[0]); Index++) {
    // ofnode_read_s32_default(..., -1) < 0 is a failure in U-Boot.
    if (!GetU32 (Fdt, Node, Fields[Index].Name, &Value) || ((INT32)Value < 0)) {
      DEBUG ((DEBUG_ERROR, "Display: timing %a has no %a\n", FdtGetName (Fdt, Node, NULL), Fields[Index].Name));
      return EFI_NOT_FOUND;
    }

    *(UINT32 *)((UINT8 *)T + Fields[Index].Offset) = Value;
  }

  T->ScreenRotate = GetU32Default (Fdt, Node, "screen-rotate", 0);
  RkModeFromTiming (T, &Desc->Mode);
  return EFI_SUCCESS;
}

/**
  The okay route's connect endpoint sits in the VOP's port; its remote is
  the DSI's input endpoint, whose reg is the crtc id (GRF VOPSEL).
**/
STATIC
VOID
FindRoute (
  IN  CONST VOID  *Fdt,
  OUT INT32       *Vop,
  OUT INT32       *Dsi,
  OUT UINT32      *CrtcId
  )
{
  INT32  Route;
  INT32  Child;
  INT32  Ep;
  INT32  Remote;
  INT32  Node;
  INT32  Depth;

  *Vop    = -1;
  *Dsi    = -1;
  *CrtcId = 0;

  Route = FdtPathOffset (Fdt, "/display-subsystem/route");
  for (Child = (Route < 0) ? -1 : FdtFirstSubnode (Fdt, Route); Child >= 0; Child = FdtNextSubnode (Fdt, Child)) {
    if (!NodeIsOkay (Fdt, Child)) {
      continue;
    }

    Ep     = GetPhandleNode (Fdt, Child, "connect");
    Remote = (Ep < 0) ? -1 : GetPhandleNode (Fdt, Ep, "remote-endpoint");
    if (Remote < 0) {
      continue;
    }

    for (Node = Remote, Depth = 0; (Node >= 0) && (Depth < 4); Node = FdtParentOffset (Fdt, Node), Depth++) {
      if (NodeIsCompatible (Fdt, Node, "rockchip,px30-mipi-dsi")) {
        break;
      }
    }

    if ((Node < 0) || (Depth == 4)) {
      DEBUG ((DEBUG_INFO, "Display: route %a is not DSI, skipped\n", FdtGetName (Fdt, Child, NULL)));
      continue;
    }

    *Dsi    = Node;
    *Vop    = FdtParentOffset (Fdt, FdtParentOffset (Fdt, Ep));
    *CrtcId = GetU32Default (Fdt, Remote, "reg", 0);
    DEBUG ((DEBUG_INFO, "Display: route %a, crtc id %u\n", FdtGetName (Fdt, Child, NULL), *CrtcId));
    return;
  }

  // No route: the okay DSI host and VOPB, like an older DT would have it.
  for (Node = FdtNodeOffsetByCompatible (Fdt, -1, "rockchip,px30-mipi-dsi"); Node >= 0;
       Node = FdtNodeOffsetByCompatible (Fdt, Node, "rockchip,px30-mipi-dsi"))
  {
    if (NodeIsOkay (Fdt, Node)) {
      *Dsi = Node;
      break;
    }
  }

  *Vop = FdtNodeOffsetByCompatible (Fdt, -1, "rockchip,px30-vop-big");
  DEBUG ((DEBUG_WARN, "Display: no okay DSI route, assuming VOPB -> DSI\n"));
}

EFI_STATUS
RkPanelParse (
  IN  CONST VOID     *Fdt,
  OUT RK_PANEL_DESC  *Panel
  )
{
  INT32        Vop;
  INT32        Dsi;
  INT32        Node;
  INT32        Phy;
  INT32        Len;
  CONST CHAR8  *CmdType;
  EFI_STATUS   Status;

  ZeroMem (Panel, sizeof (*Panel));
  Panel->Fdt   = Fdt;
  Panel->Model = FdtGetProp (Fdt, 0, "model", &Len);

  FindRoute (Fdt, &Vop, &Dsi, &Panel->CrtcId);
  if ((Dsi < 0) || (Vop < 0)) {
    DEBUG ((DEBUG_ERROR, "Display: no DSI host or VOP in the FDT\n"));
    return EFI_NOT_FOUND;
  }

  if (!NodeIsCompatible (Fdt, Vop, "rockchip,px30-vop-big") || (Panel->CrtcId != 0)) {
    // VOPL would need NPLL for its dclk, which the SD card clock uses.
    DEBUG ((DEBUG_ERROR, "Display: the panel is routed to %a (crtc %u); only VOPB is supported\n", FdtGetName (Fdt, Vop, NULL), Panel->CrtcId));
    return EFI_UNSUPPORTED;
  }

  Panel->VopBase = GetRegBase (Fdt, Vop);
  Panel->DsiBase = GetRegBase (Fdt, Dsi);
  Phy            = GetPhandleNode (Fdt, Dsi, "phys");
  Panel->PhyBase = (Phy < 0) ? 0 : GetRegBase (Fdt, Phy);
  if ((Panel->VopBase != RK_VOPB_BASE) || (Panel->DsiBase != RK_DSI_BASE) || (Panel->PhyBase != RK_DSI_PHY_BASE) ||
      !NodeIsCompatible (Fdt, Phy, "rockchip,px30-video-phy"))
  {
    DEBUG ((
      DEBUG_ERROR,
      "Display: unexpected blocks VOP 0x%lx DSI 0x%lx PHY 0x%lx\n",
      (UINT64)Panel->VopBase,
      (UINT64)Panel->DsiBase,
      (UINT64)Panel->PhyBase
      ));
    return EFI_UNSUPPORTED;
  }

  Panel->LaneRateMbps = GetU32Default (Fdt, Dsi, "rockchip,lane-rate", 0);

  for (Node = FdtFirstSubnode (Fdt, Dsi); Node >= 0; Node = FdtNextSubnode (Fdt, Node)) {
    if (NodeIsOkay (Fdt, Node) &&
        (NodeIsCompatible (Fdt, Node, "simple-panel-dsi") || NodeIsCompatible (Fdt, Node, "simple-panel")))
    {
      break;
    }
  }

  if (Node < 0) {
    DEBUG ((DEBUG_ERROR, "Display: no okay simple-panel-dsi under the DSI host\n"));
    return EFI_NOT_FOUND;
  }

  Panel->PanelNodeName   = FdtGetName (Fdt, Node, NULL);
  Panel->PanelCompatible = FdtGetProp (Fdt, Node, "compatible", &Len);

  CmdType = FdtGetProp (Fdt, Node, "rockchip,cmd-type", &Len);
  if ((CmdType != NULL) && ((AsciiStrnCmp (CmdType, "spi", 3) == 0) || (AsciiStrnCmp (CmdType, "mcu", 3) == 0))) {
    DEBUG ((DEBUG_ERROR, "Display: rockchip,cmd-type %a is not supported\n", CmdType));
    return EFI_UNSUPPORTED;
  }

  Panel->Lanes     = GetU32Default (Fdt, Node, "dsi,lanes", 4);
  Panel->Format    = GetU32Default (Fdt, Node, "dsi,format", RK_DSI_FMT_RGB888);
  Panel->ModeFlags = GetU32Default (Fdt, Node, "dsi,flags", DEFAULT_DSI_FLAGS);
  Panel->Channel   = GetU32Default (Fdt, Node, "reg", 0);
  if ((Panel->Lanes < 1) || (Panel->Lanes > 4) || (Panel->Channel > 3) || (Panel->Format > RK_DSI_FMT_RGB565)) {
    DEBUG ((DEBUG_ERROR, "Display: %u lanes / channel %u / format %u not supported\n", Panel->Lanes, Panel->Channel, Panel->Format));
    return EFI_UNSUPPORTED;
  }

  Panel->PrepareDelayMs   = GetU32Default (Fdt, Node, "prepare-delay-ms", 0);
  Panel->UnprepareDelayMs = GetU32Default (Fdt, Node, "unprepare-delay-ms", 0);
  Panel->EnableDelayMs    = GetU32Default (Fdt, Node, "enable-delay-ms", 0);
  Panel->DisableDelayMs   = GetU32Default (Fdt, Node, "disable-delay-ms", 0);
  Panel->InitDelayMs      = GetU32Default (Fdt, Node, "init-delay-ms", 0);
  Panel->ResetDelayMs     = GetU32Default (Fdt, Node, "reset-delay-ms", 0);
  Panel->BusFormat        = GetU32Default (Fdt, Node, "bus-format", RK_MEDIA_BUS_FMT_RBG888_1X24);
  Panel->Bpc              = GetU32Default (Fdt, Node, "bpc", 8);
  Panel->PowerInvert      = FdtGetProp (Fdt, Node, "power-invert", &Len) != NULL;

  Panel->InitSeq    = FdtGetProp (Fdt, Node, "panel-init-sequence", &Len);
  Panel->InitSeqLen = (Panel->InitSeq != NULL) ? (UINT32)Len : 0;
  Panel->ExitSeq    = FdtGetProp (Fdt, Node, "panel-exit-sequence", &Len);
  Panel->ExitSeqLen = (Panel->ExitSeq != NULL) ? (UINT32)Len : 0;

  GetGpio (Fdt, Node, "enable-gpios", &Panel->EnableGpio);
  GetGpio (Fdt, Node, "reset-gpios", &Panel->ResetGpio);
  GetSupply (Fdt, Node, "power-supply", &Panel->PowerSupply);
  GetSupply (Fdt, Node, "backlight-supply", &Panel->BacklightSupply);
  GetBacklight (Fdt, Node, &Panel->Backlight);

  Status = GetTiming (Fdt, Node, Panel);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Panel->Mode.HDisplay == 0) || (Panel->Mode.VDisplay == 0) || (Panel->Mode.Clock == 0) ||
      (Panel->Mode.HTotal > 0xFFF) || (Panel->Mode.VTotal > 0xFFF))
  {
    DEBUG ((DEBUG_ERROR, "Display: timing out of range\n"));
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

STATIC
VOID
LogSupply (
  IN CONST CHAR8      *What,
  IN CONST RK_SUPPLY  *Supply
  )
{
  if (!Supply->Present) {
    return;
  }

  DEBUG ((
    DEBUG_INFO,
    "Display:   %a: %a %u uV, %a",
    What,
    Supply->Name,
    Supply->MinMicroVolts,
    Supply->Fixed ? "GPIO switched" : "PMIC"
    ));
  if (Supply->Fixed && Supply->Gpio.Valid) {
    DEBUG ((DEBUG_INFO, " GPIO%u_%u%a", Supply->Gpio.Bank, Supply->Gpio.Pin, Supply->Gpio.ActiveLow ? " low" : ""));
  }

  DEBUG ((DEBUG_INFO, "\n"));
}

STATIC
VOID
LogGpio (
  IN CONST CHAR8         *What,
  IN CONST RK_GPIO_SPEC  *Gpio
  )
{
  if (Gpio->Valid) {
    DEBUG ((
      DEBUG_INFO,
      "Display:   %a: GPIO%u_%c%u (pin %u), active %a\n",
      What,
      Gpio->Bank,
      'A' + Gpio->Pin / 8,
      Gpio->Pin % 8,
      Gpio->Pin,
      Gpio->ActiveLow ? "low" : "high"
      ));
  }
}

VOID
RkPanelLog (
  IN CONST RK_PANEL_DESC  *Panel
  )
{
  CONST RK_DISPLAY_MODE    *M;
  CONST RK_BACKLIGHT_DESC  *Bl;
  RK_PANEL_CMD             Cmd;
  UINT32                   Offset;
  UINT32                   Count;
  UINT32                   DelayMs;

  M  = &Panel->Mode;
  Bl = &Panel->Backlight;

  Count   = 0;
  DelayMs = 0;
  Offset  = 0;
  while (RkPanelNextCmd (Panel->InitSeq, Panel->InitSeqLen, &Offset, &Cmd)) {
    Count++;
    DelayMs += Cmd.DelayMs;
  }

  DEBUG ((
    DEBUG_INFO,
    "Display: panel %a (%a) on '%a'\n",
    Panel->PanelNodeName,
    Panel->PanelCompatible != NULL ? Panel->PanelCompatible : "?",
    Panel->Model != NULL ? Panel->Model : "?"
    ));
  DEBUG ((
    DEBUG_INFO,
    "Display:   mode %ux%u, clock %u kHz, flags 0x%x, %u timing(s) in DT\n",
    M->HDisplay,
    M->VDisplay,
    M->Clock,
    M->Flags,
    Panel->TimingCount
    ));
  DEBUG ((DEBUG_INFO, "Display:   H %u %u %u %u  V %u %u %u %u\n", M->HDisplay, M->HSyncStart, M->HSyncEnd, M->HTotal, M->VDisplay, M->VSyncStart, M->VSyncEnd, M->VTotal));
  DEBUG ((
    DEBUG_INFO,
    "Display:   DSI %u lanes, format %u, flags 0x%x, channel %u, lane-rate %u Mbps, bus-format 0x%x\n",
    Panel->Lanes,
    Panel->Format,
    Panel->ModeFlags,
    Panel->Channel,
    Panel->LaneRateMbps,
    Panel->BusFormat
    ));
  DEBUG ((
    DEBUG_INFO,
    "Display:   delays prepare %u reset %u init %u enable %u disable %u unprepare %u ms%a\n",
    Panel->PrepareDelayMs,
    Panel->ResetDelayMs,
    Panel->InitDelayMs,
    Panel->EnableDelayMs,
    Panel->DisableDelayMs,
    Panel->UnprepareDelayMs,
    Panel->PowerInvert ? ", power-invert" : ""
    ));
  DEBUG ((
    DEBUG_INFO,
    "Display:   init sequence %u commands (%u bytes, %u ms of delays), exit sequence %u bytes\n",
    Count,
    Panel->InitSeqLen,
    DelayMs,
    Panel->ExitSeqLen
    ));
  LogGpio ("reset", &Panel->ResetGpio);
  LogGpio ("enable", &Panel->EnableGpio);
  LogSupply ("power", &Panel->PowerSupply);
  LogSupply ("backlight power", &Panel->BacklightSupply);
  if (Bl->Present) {
    DEBUG ((
      DEBUG_INFO,
      "Display:   backlight PWM 0x%lx (block %u ch %u), period %u ns%a, level %u of %u, %u pin(s)\n",
      (UINT64)Bl->PwmBase,
      Bl->PwmBlock,
      Bl->Channel,
      Bl->PeriodNs,
      Bl->Inverted ? " inverted" : "",
      Bl->DefaultLevel,
      Bl->MaxLevel,
      Bl->PinCount
      ));
    LogGpio ("backlight enable", &Bl->Enable);
    LogSupply ("backlight PWM power", &Bl->Supply);
  } else {
    DEBUG ((DEBUG_WARN, "Display:   no usable backlight\n"));
  }
}

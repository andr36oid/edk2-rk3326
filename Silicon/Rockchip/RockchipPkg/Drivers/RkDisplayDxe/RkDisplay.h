/** @file
  RkDisplayDxe internals: the panel as the screen FDT describes it, the
  values computed from it, and the steps of the bring-up.

  The model follows U-Boot's Rockchip DRM driver (drivers/video/drm:
  rockchip_display.c, rockchip_vop.c, dw_mipi_dsi.c, inno_video_combo_phy.c,
  rockchip_panel.c, drm_mipi_dsi.c, video/pwm_backlight.c, pwm/rk_pwm.c),
  reduced to the one path every andr36oid console uses: VOPB -> DW MIPI DSI
  -> Inno video PHY -> DSI panel with a PWM backlight.

  SPDX-License-Identifier: GPL-2.0+
**/

#ifndef RK_DISPLAY_H_
#define RK_DISPLAY_H_

#include <Uefi.h>

//
// Fixed PX30 blocks. The DT gives the same addresses; the parser checks them.
//
#define RK_VOPB_BASE        0xFF460000UL
#define RK_VOPL_BASE        0xFF470000UL
#define RK_DSI_BASE         0xFF450000UL
#define RK_DSI_PHY_BASE     0xFF2E0000UL
#define RK_GRF_BASE         0xFF140000UL
#define RK_PMU_BASE         0xFF000000UL

//
// struct drm_display_mode flags (U-Boot drm_modes.h values).
//
#define RK_MODE_PHSYNC      BIT0
#define RK_MODE_NHSYNC      BIT1
#define RK_MODE_PVSYNC      BIT2
#define RK_MODE_NVSYNC      BIT3
#define RK_MODE_XMIRROR     BIT28
#define RK_MODE_YMIRROR     BIT29
#define RK_MODE_PPIXDATA    BIT31

//
// dsi,flags (MIPI_DSI_MODE_* in drm_mipi_dsi.h).
//
#define RK_DSI_MODE_VIDEO           BIT0
#define RK_DSI_MODE_BURST           BIT1
#define RK_DSI_MODE_SYNC_PULSE      BIT2
#define RK_DSI_MODE_HFP             BIT5
#define RK_DSI_MODE_HBP             BIT6
#define RK_DSI_MODE_EOT_PACKET      BIT9
#define RK_DSI_CLOCK_NON_CONTINUOUS BIT10
#define RK_DSI_MODE_LPM             BIT11

#define RK_DSI_FMT_RGB888           0
#define RK_DSI_FMT_RGB666           1
#define RK_DSI_FMT_RGB666_PACKED    2
#define RK_DSI_FMT_RGB565           3

#define RK_MEDIA_BUS_FMT_RBG888_1X24  0x100E

//
// MIPI DSI data types the panel sequences use.
//
#define MIPI_DSI_GENERIC_SHORT_WRITE_0  0x03
#define MIPI_DSI_GENERIC_SHORT_WRITE_1  0x13
#define MIPI_DSI_GENERIC_SHORT_WRITE_2  0x23
#define MIPI_DSI_GENERIC_LONG_WRITE     0x29
#define MIPI_DSI_DCS_SHORT_WRITE        0x05
#define MIPI_DSI_DCS_SHORT_WRITE_PARAM  0x15
#define MIPI_DSI_DCS_LONG_WRITE         0x39

typedef struct {
  UINT32    Clock;        // kHz
  UINT32    HDisplay;
  UINT32    HSyncStart;
  UINT32    HSyncEnd;
  UINT32    HTotal;
  UINT32    VDisplay;
  UINT32    VSyncStart;
  UINT32    VSyncEnd;
  UINT32    VTotal;
  UINT32    Flags;        // RK_MODE_*
} RK_DISPLAY_MODE;

//
// One display-timings entry as the DT gives it.
//
typedef struct {
  UINT32    ClockFrequency;   // Hz
  UINT32    HActive;
  UINT32    HFrontPorch;
  UINT32    HSyncLen;
  UINT32    HBackPorch;
  UINT32    VActive;
  UINT32    VFrontPorch;
  UINT32    VSyncLen;
  UINT32    VBackPorch;
  UINT32    HSyncActive;
  UINT32    VSyncActive;
  UINT32    PixelClkActive;
  UINT32    ScreenRotate;
} RK_DT_TIMING;

typedef struct {
  BOOLEAN    Valid;
  UINT8      Bank;
  UINT8      Pin;
  BOOLEAN    ActiveLow;
} RK_GPIO_SPEC;

typedef struct {
  BOOLEAN         Present;
  INT32           Node;            // FDT offset, for RkPmicSetRegulator
  CONST CHAR8     *Name;           // regulator-name
  BOOLEAN         Fixed;           // regulator-fixed: switched by Gpio
  RK_GPIO_SPEC    Gpio;
  BOOLEAN         EnableActiveHigh;  // regulator-fixed: on as soon as U-Boot probes it
  UINT32          StartupDelayUs;
  UINT32          MinMicroVolts;
  CONST CHAR8     *SharedWith;     // another user of the rail: never switched off
} RK_SUPPLY;

typedef struct {
  UINT8    Bank;
  UINT8    Pin;
  UINT8    Function;
  INT8     Pull;                   // GPIO_PIN_PULL value, -1 leave alone
} RK_PINMUX;

#define RK_MAX_PINS  4

typedef struct {
  BOOLEAN         Present;
  UINTN           PwmBase;         // this channel's registers
  UINT8           PwmBlock;        // clock block: 0 = pwm0-3, 1 = pwm4-7
  UINT32          Channel;
  UINT32          PeriodNs;
  BOOLEAN         Inverted;
  UINT32          DefaultLevel;
  UINT32          MaxLevel;
  RK_GPIO_SPEC    Enable;
  RK_SUPPLY       Supply;
  RK_PINMUX       Pins[RK_MAX_PINS];
  UINT32          PinCount;
} RK_BACKLIGHT_DESC;

typedef struct {
  CONST VOID           *Fdt;
  CONST CHAR8          *Model;
  CONST CHAR8          *PanelCompatible;
  CONST CHAR8          *PanelNodeName;

  UINTN                VopBase;
  UINT32               CrtcId;         // GRF VOPSEL: 0 VOPB, 1 VOPL
  UINTN                DsiBase;
  UINTN                PhyBase;

  UINT32               Lanes;
  UINT32               Format;
  UINT32               ModeFlags;
  UINT32               Channel;
  UINT32               LaneRateMbps;   // rockchip,lane-rate on the host, 0 = compute

  RK_DT_TIMING         Timing;
  UINT32               TimingCount;
  RK_DISPLAY_MODE      Mode;
  UINT32               BusFormat;
  UINT32               Bpc;

  UINT32               PrepareDelayMs;
  UINT32               UnprepareDelayMs;
  UINT32               EnableDelayMs;
  UINT32               DisableDelayMs;
  UINT32               ResetDelayMs;
  UINT32               InitDelayMs;
  BOOLEAN              PowerInvert;

  CONST UINT8          *InitSeq;
  UINT32               InitSeqLen;
  CONST UINT8          *ExitSeq;
  UINT32               ExitSeqLen;

  RK_GPIO_SPEC         EnableGpio;
  RK_GPIO_SPEC         ResetGpio;
  RK_SUPPLY            PowerSupply;
  RK_SUPPLY            BacklightSupply;
  RK_BACKLIGHT_DESC    Backlight;
} RK_PANEL_DESC;

//
// Everything computed from the mode before any register is touched.
//
typedef struct {
  UINT64    LaneRateBps;       // what the DSI host asks the PHY for
  UINT64    PhyRateBps;        // what the PHY PLL gives
  UINT8     PhyPrediv;
  UINT16    PhyFbdiv;
  UINT32    LaneMbps;
  UINT32    EscClkDiv;         // DSI CLKMGR TX escape divider
  UINT32    HlineLbcc;
  UINT32    HsaLbcc;
  UINT32    HbpLbcc;
  UINT32    Vsa;
  UINT32    Vbp;
  UINT32    Vfp;
  // Inno PHY lane timing counters
  UINT32    Lpx;
  UINT32    HsPrepare;
  UINT32    ClkHsZero;
  UINT32    DataHsZero;
  UINT32    HsTrail;
  UINT32    HsExit;
  UINT32    ClkPost;
  UINT32    ClkPre;
  UINT32    Wakeup;
  UINT32    TaGo;
  UINT32    TaSure;
  UINT32    TaWait;
} RK_DSI_CONFIG;

typedef struct {
  UINT32    HTotalPw;          // DSP_HTOTAL_HS_END
  UINT32    HActStEnd;
  UINT32    VTotalPw;
  UINT32    VActStEnd;
  UINT32    LineFlag;
  UINT32    PinPol;            // mipi_pin_pol
  UINT32    DclkInv;           // mipi_dclk_pol
  UINT32    DspInfo;           // WIN1 size, full screen
  UINT32    DspSt;             // WIN1 start
  UINT32    Vir;               // WIN1 stride in words
} RK_VOP_CONFIG;

typedef struct {
  UINT8          Type;
  UINT8          DelayMs;
  UINT8          Length;
  CONST UINT8    *Payload;
} RK_PANEL_CMD;

//
// DisplayMath.c: no register access, host-tested against U-Boot.
//
VOID
RkModeFromTiming (
  IN  CONST RK_DT_TIMING  *Timing,
  OUT RK_DISPLAY_MODE     *Mode
  );

UINT32
RkDsiFormatBpp (
  IN UINT32  Format
  );

UINT64
RkDsiLaneRate (
  IN CONST RK_DISPLAY_MODE  *Mode,
  IN UINT32                 Format,
  IN UINT32                 Lanes,
  IN UINT32                 LaneRateMbps
  );

UINT64
RkInnoPllRoundRate (
  IN  UINT64  ParentRate,
  IN  UINT64  Rate,
  OUT UINT8   *Prediv,
  OUT UINT16  *Fbdiv
  );

EFI_STATUS
RkDsiComputeConfig (
  IN  CONST RK_PANEL_DESC  *Panel,
  OUT RK_DSI_CONFIG        *Config
  );

VOID
RkVopComputeConfig (
  IN  CONST RK_DISPLAY_MODE  *Mode,
  IN  UINT32                 Width,
  IN  UINT32                 Height,
  IN  UINT32                 BitsPerPixel,
  OUT RK_VOP_CONFIG          *Config
  );

UINT32
RkBacklightDutyNs (
  IN CONST RK_BACKLIGHT_DESC  *Backlight
  );

UINT32
RkPwmCount (
  IN UINT64  ClockHz,
  IN UINT32  Ns
  );

BOOLEAN
RkPanelNextCmd (
  IN     CONST UINT8   *Seq,
  IN     UINT32        SeqLen,
  IN OUT UINT32        *Offset,
  OUT    RK_PANEL_CMD  *Cmd
  );

UINT8
RkPanelCmdMsgType (
  IN UINT8  CmdType,
  IN UINT8  Length
  );

UINT32
RkDsiPacketHeader (
  IN UINT8        Channel,
  IN UINT8        MsgType,
  IN CONST UINT8  *Payload,
  IN UINT32       Length
  );

BOOLEAN
RkDsiMsgTypeIsLong (
  IN UINT8  MsgType
  );

//
// DisplayFdt.c
//
EFI_STATUS
RkPanelParse (
  IN  CONST VOID     *Fdt,
  OUT RK_PANEL_DESC  *Panel
  );

VOID
RkPanelLog (
  IN CONST RK_PANEL_DESC  *Panel
  );

//
// Hardware steps (Vop.c, Dsi.c, InnoPhy.c, Panel.c, Backlight.c, Display.c).
//
typedef struct {
  RK_PANEL_DESC    Panel;
  RK_DSI_CONFIG    Dsi;
  RK_VOP_CONFIG    Vop;
  UINT32           VopRegs[0x170 / 4];   // U-Boot's regsbak: VOP writes are cached read-modify-write
  UINT64           DclkHz;
  UINT64           PwmClockHz;
  EFI_PHYSICAL_ADDRESS  FbBase;
  UINT32           FbWidth;
  UINT32           FbHeight;
  UINT32           FbStride;
  UINT32           FbBpp;
} RK_DISPLAY;

VOID
RkVopInit (
  IN OUT RK_DISPLAY  *Display
  );

VOID
RkVopSetPlane (
  IN OUT RK_DISPLAY  *Display
  );

VOID
RkVopEnable (
  IN OUT RK_DISPLAY  *Display
  );

VOID
RkVopStandby (
  IN UINTN    VopBase,
  IN BOOLEAN  WaitFrame
  );

VOID
RkVopLogRegs (
  IN UINTN  VopBase
  );

VOID
RkDsiVopRouting (
  IN UINT32  CrtcId
  );

EFI_STATUS
RkDsiPrepare (
  IN RK_DISPLAY  *Display
  );

VOID
RkDsiEnable (
  IN RK_DISPLAY  *Display
  );

VOID
RkDsiDisable (
  IN UINTN  DsiBase
  );

VOID
RkDsiPostDisable (
  IN UINTN  DsiBase
  );

EFI_STATUS
RkDsiTransfer (
  IN UINTN        DsiBase,
  IN UINT32       ModeFlags,
  IN UINT8        Channel,
  IN UINT8        MsgType,
  IN CONST UINT8  *Payload,
  IN UINT32       Length
  );

VOID
RkInnoPhyPowerOn (
  IN UINTN                PhyBase,
  IN CONST RK_DSI_CONFIG  *Config
  );

VOID
RkInnoPhyPowerOff (
  IN UINTN  PhyBase
  );

VOID
RkPanelGpioInit (
  IN CONST RK_PANEL_DESC  *Panel
  );

EFI_STATUS
RkPanelPrepare (
  IN CONST RK_PANEL_DESC  *Panel
  );

VOID
RkPanelUnprepare (
  IN CONST RK_PANEL_DESC  *Panel
  );

VOID
RkGpioSet (
  IN CONST RK_GPIO_SPEC  *Gpio,
  IN BOOLEAN             Active
  );

EFI_STATUS
RkSupplySet (
  IN CONST VOID       *Fdt,
  IN CONST RK_SUPPLY  *Supply,
  IN BOOLEAN          Enable
  );

EFI_STATUS
RkBacklightEnable (
  IN OUT RK_DISPLAY  *Display
  );

VOID
RkBacklightDisable (
  IN RK_DISPLAY  *Display
  );

EFI_STATUS
RkDisplayEnable (
  IN OUT RK_DISPLAY  *Display
  );

VOID
RkDisplayShutdown (
  IN RK_DISPLAY  *Display
  );

#endif

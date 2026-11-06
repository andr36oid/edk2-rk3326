/** @file
  Order of the bring-up and the shutdown, as U-Boot's rockchip_display.c
  runs them (display_init, display_set_plane, display_enable,
  display_disable), plus what U-Boot leaves to others: the VO power domain
  (kernel pm_domains.c, px30 PD_VO) and the soft resets before a re-init.

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Library/CruLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include "RkDisplay.h"

// PMU power domain registers (kernel px30_pmu, DOMAIN_PX30 PD_VO)
#define PMU_PWRDN_CON     0x18
#define PMU_PWRDN_ST      0x20
#define PMU_BUS_IDLE_REQ  0x64
#define PMU_BUS_IDLE_ST   0x6C
#define PD_VO_PWR         BIT13
#define PD_VO_REQ         BIT7
#define PD_VO_ACK         BIT7
#define PD_VO_IDLE        BIT23

// CRU soft resets (px30-cru.h SRST_* / 16): all in SOFTRST_CON3
#define SRST_CON_VO        3
#define SRST_VOPB_H_BIT    4
#define SRST_VOPB_BIT      5
#define SRST_DSI_HOST_BIT  13
#define SRST_DSI_PHY_BIT   14

STATIC
EFI_STATUS
PmuWait (
  IN UINT32   Reg,
  IN UINT32   Mask,
  IN UINT32   Want
  )
{
  UINTN  Waited;

  for (Waited = 0; Waited < 10000; Waited++) {
    if ((MmioRead32 (RK_PMU_BASE + Reg) & Mask) == Want) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
  }

  return EFI_TIMEOUT;
}

/**
  rockchip_pd_power(PD_VO, on): power up, wait for the status, then take
  the bus out of idle and wait for ack and idle to clear. U-Boot never
  touches it (the domain comes up powered); this is for a stage that left
  it off.
**/
STATIC
EFI_STATUS
PowerOnVo (
  VOID
  )
{
  UINT32  Status;
  UINT32  Idle;

  Status = MmioRead32 (RK_PMU_BASE + PMU_PWRDN_ST);
  Idle   = MmioRead32 (RK_PMU_BASE + PMU_BUS_IDLE_ST);
  DEBUG ((DEBUG_INFO, "Display: PD_VO %a, bus %a (PWRDN_ST 0x%08x IDLE_ST 0x%08x)\n",
    (Status & PD_VO_PWR) ? "off" : "on", (Idle & (PD_VO_ACK | PD_VO_IDLE)) ? "idle" : "active", Status, Idle));
  if (((Status & PD_VO_PWR) == 0) && ((Idle & (PD_VO_ACK | PD_VO_IDLE)) == 0)) {
    return EFI_SUCCESS;
  }

  MmioWrite32 (RK_PMU_BASE + PMU_PWRDN_CON, PD_VO_PWR << 16);
  if (EFI_ERROR (PmuWait (PMU_PWRDN_ST, PD_VO_PWR, 0))) {
    DEBUG ((DEBUG_ERROR, "Display: PD_VO does not power up\n"));
    return EFI_TIMEOUT;
  }

  MmioWrite32 (RK_PMU_BASE + PMU_BUS_IDLE_REQ, PD_VO_REQ << 16);
  if (EFI_ERROR (PmuWait (PMU_BUS_IDLE_ST, PD_VO_ACK | PD_VO_IDLE, 0))) {
    DEBUG ((DEBUG_ERROR, "Display: PD_VO bus stays idle (0x%08x)\n", MmioRead32 (RK_PMU_BASE + PMU_BUS_IDLE_ST)));
    return EFI_TIMEOUT;
  }

  DEBUG ((DEBUG_INFO, "Display: PD_VO powered up\n"));
  return EFI_SUCCESS;
}

STATIC
VOID
ResetVoBlocks (
  VOID
  )
{
  CruAssertSoftReset (SRST_CON_VO, SRST_VOPB_H_BIT);
  CruAssertSoftReset (SRST_CON_VO, SRST_VOPB_BIT);
  CruAssertSoftReset (SRST_CON_VO, SRST_DSI_HOST_BIT);
  CruAssertSoftReset (SRST_CON_VO, SRST_DSI_PHY_BIT);
  MicroSecondDelay (10);
  CruDeassertSoftReset (SRST_CON_VO, SRST_DSI_PHY_BIT);
  CruDeassertSoftReset (SRST_CON_VO, SRST_DSI_HOST_BIT);
  CruDeassertSoftReset (SRST_CON_VO, SRST_VOPB_BIT);
  CruDeassertSoftReset (SRST_CON_VO, SRST_VOPB_H_BIT);
  MicroSecondDelay (10);
  DEBUG ((DEBUG_INFO, "Display: VOPB, DSI host and DSI PHY reset\n"));
}

/**
  From a dark (or just shut down) display to the panel showing FbBase.
  The FB fields of Display must be set.
**/
EFI_STATUS
RkDisplayEnable (
  IN OUT RK_DISPLAY  *Display
  )
{
  RK_PANEL_DESC        *Panel;
  CONST RK_DSI_CONFIG  *Dsi;
  EFI_STATUS           Status;
  EFI_STATUS           PhyStatus;

  Panel = &Display->Panel;
  Dsi   = &Display->Dsi;

  Status = RkDsiComputeConfig (Panel, &Display->Dsi);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Display: no PHY PLL setting for this lane rate\n"));
    return Status;
  }

  RkVopComputeConfig (&Panel->Mode, Display->FbWidth, Display->FbHeight, Display->FbBpp, &Display->Vop);
  Display->Vop.Vir = Display->FbStride / 4;
  DEBUG ((
    DEBUG_INFO,
    "Display: lane rate %lu bps asked, PHY PLL prediv %u fbdiv %u = %lu bps (%u Mbps), esc div %u\n",
    Dsi->LaneRateBps,
    Dsi->PhyPrediv,
    Dsi->PhyFbdiv,
    Dsi->PhyRateBps,
    Dsi->LaneMbps,
    Dsi->EscClkDiv
    ));
  DEBUG ((
    DEBUG_INFO,
    "Display: lbcc hline %u hsa %u hbp %u, lines vsa %u vbp %u vfp %u\n",
    Dsi->HlineLbcc,
    Dsi->HsaLbcc,
    Dsi->HbpLbcc,
    Dsi->Vsa,
    Dsi->Vbp,
    Dsi->Vfp
    ));
  DEBUG ((
    DEBUG_INFO,
    "Display: PHY lpx %u prepare %u zero clk %u data %u trail %u exit %u post %u pre %u wakeup %u ta %u/%u/%u\n",
    Dsi->Lpx,
    Dsi->HsPrepare,
    Dsi->ClkHsZero,
    Dsi->DataHsZero,
    Dsi->HsTrail,
    Dsi->HsExit,
    Dsi->ClkPost,
    Dsi->ClkPre,
    Dsi->Wakeup,
    Dsi->TaGo,
    Dsi->TaSure,
    Dsi->TaWait
    ));

  // Clocks and power first, so every block below answers.
  CruEnableDisplayClocks ();
  Status = PowerOnVo ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // What the U-Boot probes do: pins to GPIO, panel held off.
  RkPanelGpioInit (Panel);

  // rockchip_vop_init sets the dclk first.
  Display->DclkHz = CruSetVopbDclk (Panel->Mode.Clock * 1000);
  DEBUG ((DEBUG_INFO, "Display: dclk %lu Hz for %u kHz\n", Display->DclkHz, Panel->Mode.Clock));
  if (Display->DclkHz == 0) {
    return EFI_DEVICE_ERROR;
  }

  RkVopInit (Display);
  RkVopSetPlane (Display);

  // display_enable: connector prepare, panel prepare, crtc enable,
  // connector enable, panel enable.
  PhyStatus = RkDsiPrepare (Display);
  Status    = RkPanelPrepare (Panel);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Display: panel init sequence failed: %r\n", Status));
  }

  RkVopEnable (Display);
  RkDsiEnable (Display);

  MicroSecondDelay (Panel->EnableDelayMs * 1000);
  if (EFI_ERROR (RkBacklightEnable (Display))) {
    DEBUG ((DEBUG_WARN, "Display: backlight not switched on\n"));
  }

  RkVopLogRegs (Panel->VopBase);
  if (EFI_ERROR (PhyStatus)) {
    return PhyStatus;
  }

  return Status;
}

/**
  display_disable on what the previous stage set up, then the blocks are
  reset so RkDisplayEnable starts from their power-on state.
**/
VOID
RkDisplayShutdown (
  IN RK_DISPLAY  *Display
  )
{
  RK_PANEL_DESC  *Panel;

  Panel = &Display->Panel;
  DEBUG ((DEBUG_INFO, "Display: shutting the screen down\n"));
  RkVopLogRegs (Panel->VopBase);

  // panel disable
  RkBacklightDisable (Display);
  MicroSecondDelay (Panel->DisableDelayMs * 1000);

  RkDsiDisable (Panel->DsiBase);
  RkVopStandby (Panel->VopBase, TRUE);

  // panel unprepare sends the exit sequence through the host, still up
  // in command mode
  RkPanelUnprepare (Panel);
  RkDsiPostDisable (Panel->DsiBase);
  RkInnoPhyPowerOff (Panel->PhyBase);

  ResetVoBlocks ();
  DEBUG ((DEBUG_INFO, "Display: screen is off\n"));
}

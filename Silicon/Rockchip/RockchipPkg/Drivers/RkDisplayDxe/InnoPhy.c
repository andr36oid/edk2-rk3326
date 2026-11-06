/** @file
  Innosilicon video combo PHY (PX30) in MIPI D-PHY mode.

  Ported from U-Boot drivers/video/drm/inno_video_combo_phy.c
  (inno_video_phy_power_on, inno_video_phy_mipi_mode_enable,
  inno_video_phy_power_off). The PLL dividers and lane counters come from
  DisplayMath.c.

  The register address is split: bits 7:5 pick the part (analog, digital,
  clock lane, data lanes 0-3, LVDS), bits 4:0 the register in it.

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include "RkDisplay.h"

#define PART_ANALOG      0
#define PART_DIGITAL     1
#define PART_CLOCK_LANE  2
#define PART_DATA3_LANE  6
#define PART_LVDS        7

#define PHY_REG(First, Second)  ((((First) & 0x7) << 5 | ((Second) & 0x1F)) << 2)

STATIC
VOID
PhyUpdate (
  IN UINTN  Base,
  IN UINT8  First,
  IN UINT8  Second,
  IN UINT8  Mask,
  IN UINT8  Value
  )
{
  UINTN  Reg;

  Reg = Base + PHY_REG (First, Second);
  MmioWrite32 (Reg, (MmioRead32 (Reg) & ~(UINT32)Mask) | (Value & Mask));
}

VOID
RkInnoPhyPowerOn (
  IN UINTN                PhyBase,
  IN CONST RK_DSI_CONFIG  *Cfg
  )
{
  UINT8  Part;
  UINT8  HsZero;

  // Bandgap on, power work enable
  PhyUpdate (PhyBase, PART_ANALOG, 0x00, BIT7, 0);
  PhyUpdate (PhyBase, PART_ANALOG, 0x00, 0x3, 0x1);

  // MIPI mode
  PhyUpdate (PhyBase, PART_LVDS, 0x03, 0x7, BIT0);

  // PLL: prediv 4:0 and fbdiv bit 8 in reg03, fbdiv 7:0 in reg04
  PhyUpdate (PhyBase, PART_ANALOG, 0x03, 0x1F, Cfg->PhyPrediv);
  PhyUpdate (PhyBase, PART_ANALOG, 0x03, BIT5, (UINT8)((Cfg->PhyFbdiv >> 8) << 5));
  PhyUpdate (PhyBase, PART_ANALOG, 0x04, 0xFF, (UINT8)Cfg->PhyFbdiv);

  // PLL and LDO on, then pulse the analog and digital resets
  PhyUpdate (PhyBase, PART_ANALOG, 0x01, BIT1 | BIT0, 0);
  PhyUpdate (PhyBase, PART_ANALOG, 0x01, BIT2, BIT2);
  MicroSecondDelay (1);
  PhyUpdate (PhyBase, PART_ANALOG, 0x01, BIT2, 0);
  PhyUpdate (PhyBase, PART_DIGITAL, 0x00, BIT0, 0);
  MicroSecondDelay (1);
  PhyUpdate (PhyBase, PART_DIGITAL, 0x00, BIT0, BIT0);

  for (Part = PART_CLOCK_LANE; Part <= PART_DATA3_LANE; Part++) {
    HsZero = (UINT8)((Part == PART_CLOCK_LANE) ? Cfg->ClkHsZero : Cfg->DataHsZero);
    PhyUpdate (PhyBase, Part, 0x05, 0x3F, (UINT8)Cfg->Lpx);
    PhyUpdate (PhyBase, Part, 0x06, 0x7F, (UINT8)Cfg->HsPrepare);
    PhyUpdate (PhyBase, Part, 0x07, 0x3F, HsZero);
    PhyUpdate (PhyBase, Part, 0x08, 0x7F, (UINT8)Cfg->HsTrail);
    PhyUpdate (PhyBase, Part, 0x09, 0x1F, (UINT8)Cfg->HsExit);
    PhyUpdate (PhyBase, Part, 0x0A, 0x0F, (UINT8)Cfg->ClkPost);
    PhyUpdate (PhyBase, Part, 0x0E, 0x0F, (UINT8)Cfg->ClkPre);
    PhyUpdate (PhyBase, Part, 0x0C, 0x03, (UINT8)(Cfg->Wakeup >> 8));
    PhyUpdate (PhyBase, Part, 0x0D, 0xFF, (UINT8)Cfg->Wakeup);
    PhyUpdate (PhyBase, Part, 0x10, 0x3F, (UINT8)Cfg->TaGo);
    PhyUpdate (PhyBase, Part, 0x11, 0x3F, (UINT8)Cfg->TaSure);
    PhyUpdate (PhyBase, Part, 0x12, 0x3F, (UINT8)Cfg->TaWait);
  }

  // All lanes on in the analog part (clock, data 3..0)
  PhyUpdate (PhyBase, PART_ANALOG, 0x00, 0x7C, 0x7C);

  DEBUG ((
    DEBUG_INFO,
    "InnoPhy: readback A00 0x%02x A01 0x%02x A03 0x%02x A04 0x%02x D00 0x%02x L03 0x%02x\n",
    MmioRead32 (PhyBase + PHY_REG (PART_ANALOG, 0x00)) & 0xFF,
    MmioRead32 (PhyBase + PHY_REG (PART_ANALOG, 0x01)) & 0xFF,
    MmioRead32 (PhyBase + PHY_REG (PART_ANALOG, 0x03)) & 0xFF,
    MmioRead32 (PhyBase + PHY_REG (PART_ANALOG, 0x04)) & 0xFF,
    MmioRead32 (PhyBase + PHY_REG (PART_DIGITAL, 0x00)) & 0xFF,
    MmioRead32 (PhyBase + PHY_REG (PART_LVDS, 0x03)) & 0xFF
    ));
}

VOID
RkInnoPhyPowerOff (
  IN UINTN  PhyBase
  )
{
  PhyUpdate (PhyBase, PART_ANALOG, 0x00, 0x7C, 0);                 // lanes off
  PhyUpdate (PhyBase, PART_ANALOG, 0x01, BIT1 | BIT0, BIT1 | BIT0);// LDO, PLL down
  PhyUpdate (PhyBase, PART_ANALOG, 0x00, 0x3, 0x2);                // power work off
  PhyUpdate (PhyBase, PART_ANALOG, 0x00, BIT7, BIT7);              // bandgap down
  PhyUpdate (PhyBase, PART_LVDS, 0x0B, 0xF8, 0);
  PhyUpdate (PhyBase, PART_LVDS, 0x01, BIT7, 0);
  PhyUpdate (PhyBase, PART_LVDS, 0x0B, BIT2 | BIT0, BIT2 | BIT0);
}

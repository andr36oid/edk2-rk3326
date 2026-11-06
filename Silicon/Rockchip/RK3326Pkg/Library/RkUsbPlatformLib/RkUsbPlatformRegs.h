/** @file
  PX30 registers the USB host port needs, and the values the Linux phy and
  clock drivers write for them. Rockchip GRF/CRU registers take a write-enable
  mask in the upper 16 bits, so every value here is (mask << 16) | value.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef RK_USB_PLATFORM_REGS_H_
#define RK_USB_PLATFORM_REGS_H_

#define HIWORD(Mask, Val)  ((((UINT32)(Mask)) << 16) | ((UINT32)(Val) & (Mask)))

#define RK_USB_OTG_BASE        0xFF300000ULL

// Power management unit (px30.dtsi pmu, rockchip_pmu_info px30_pmu)
#define PMU_BASE               0xFF000000UL
#define PMU_PWRDN_CON          (PMU_BASE + 0x18)
#define PMU_PWRDN_ST           (PMU_BASE + 0x20)
#define PMU_BUS_IDLE_REQ       (PMU_BASE + 0x64)
#define PMU_BUS_IDLE_ST        (PMU_BASE + 0x6C)
#define PD_USB_PWR_BIT         5     // PWRDN_CON / PWRDN_ST (1 = off)
#define PD_USB_IDLE_BIT        10    // idle request / ack

// CRU gates (a set bit stops the clock), clk-px30.c
#define CRU_GATE7_HCLK_USB     (1U << 2)
#define CRU_GATE7_OTG_ADP      (1U << 3)
#define CRU_GATE7_USB_NIU      (1U << 4)
#define CRU_GATE7_HCLK_OTG     (1U << 5)
#define CRU_GATE16_PCLK_USBGRF (1U << 6)

// CRU soft resets, px30-cru.h: SRST_USB_NIU_H 65 .. SRST_USB2OTG_ADP 68
#define CRU_SOFTRST_USB_CON    4
#define SRST_USB2OTG_H_BIT     2
#define SRST_USB2OTG_BIT       3
#define SRST_USB2OTG_ADP_BIT   4

// PMU CRU: clk_usbphy_ref (SCLK_USBPHY_REF), the PHY's input clock
#define PMUCRU_CLKSEL_CON(n)   (PMUCRU_BASE + 0x40 + (n) * 4)   // PMUCRU_CLKGATE_CON is in CruLib.h
#define PMUCRU_USBPHY_SEL_BIT  6     // CLKSEL_CON(2): 0 = xin24m
#define PMUCRU_USBPHY_GATE_BIT 9     // CLKGATE_CON(1)

// USB2 PHY GRF (px30.dtsi usb2phy_grf; the phy node sits at +0x100)
#define USB2PHY_GRF            0xFF2C0000UL

// rk3328_phy_cfgs, which px30 uses ("rockchip,px30-usb2phy" falls back to it)
#define PHY_TUNE0_OFF          0x2C
#define PHY_TUNE0_VAL          0xFFFF0400U
#define PHY_TUNE1_OFF          0x00
#define PHY_TUNE1_VAL          0x00070004U
#define PHY_TUNE2_OFF          0x30
#define PHY_TUNE2_VAL          0x00070004U
#define PHY_TUNE3_OFF          0x18
#define PHY_TUNE3_VAL          0x00040000U
#define PHY_CLKOUT_OFF         0x108                      // clkout_ctl {0x108, 4, 4, 1, 0}
#define PHY_CLKOUT_ENABLE      HIWORD (1U << 4, 0)
#define PHY_OTG_CTL_OFF        0x100
#define PHY_SUSPEND_RELEASE    HIWORD (0x1FF, 0)          // phy_sus {0x100, 8, 0, 0, 0x1d1}, disable value
#define PHY_IDDIG_OUTPUT_LOW   HIWORD (1U << 10, 0)       // iddig_output {0x100, 10, 10, 0, 1}: low = A device
#define PHY_IDDIG_EN           HIWORD (1U << 9, 1U << 9)  // iddig_en {0x100, 9, 9, 0, 1}: GRF drives the ID
#define PHY_VBUS_DET_OFF       0x1C                       // vbus_det_en {0x1c, 15, 15, 1, 0}
#define PHY_VBUS_DET_DISABLE   HIWORD (1U << 15, 1U << 15)

#endif

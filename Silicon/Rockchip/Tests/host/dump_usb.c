// Prints the PHY/CRU values RkUsbPlatformRegs.h holds, for check-usb-vs-kernel.py.
#include <Uefi.h>
#include <stdio.h>
#include "RkUsbPlatformRegs.h"

int main (void)
{
  printf ("tune 0x%x 0x%08x\n", PHY_TUNE0_OFF, PHY_TUNE0_VAL);
  printf ("tune 0x%x 0x%08x\n", PHY_TUNE1_OFF, PHY_TUNE1_VAL);
  printf ("tune 0x%x 0x%08x\n", PHY_TUNE2_OFF, PHY_TUNE2_VAL);
  printf ("tune 0x%x 0x%08x\n", PHY_TUNE3_OFF, PHY_TUNE3_VAL);
  printf ("clkout_ctl 0x%x 0x%08x\n", PHY_CLKOUT_OFF, (unsigned)PHY_CLKOUT_ENABLE);
  printf ("phy_sus 0x%x 0x%08x\n", PHY_OTG_CTL_OFF, (unsigned)PHY_SUSPEND_RELEASE);
  printf ("iddig_output 0x%x 0x%08x\n", PHY_OTG_CTL_OFF, (unsigned)PHY_IDDIG_OUTPUT_LOW);
  printf ("iddig_en 0x%x 0x%08x\n", PHY_OTG_CTL_OFF, (unsigned)PHY_IDDIG_EN);
  printf ("vbus_det_en 0x%x 0x%08x\n", PHY_VBUS_DET_OFF, (unsigned)PHY_VBUS_DET_DISABLE);
  printf ("pd_usb pwr %d idle %d\n", PD_USB_PWR_BIT, PD_USB_IDLE_BIT);
  printf ("gate7 0x%x\n", (unsigned)(CRU_GATE7_HCLK_USB | CRU_GATE7_OTG_ADP | CRU_GATE7_USB_NIU | CRU_GATE7_HCLK_OTG));
  printf ("softrst %d %d %d %d\n", CRU_SOFTRST_USB_CON, SRST_USB2OTG_H_BIT, SRST_USB2OTG_BIT, SRST_USB2OTG_ADP_BIT);
  return 0;
}

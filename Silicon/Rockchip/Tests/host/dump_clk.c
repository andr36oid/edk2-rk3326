// Prints the clock tables compiled into the driver, for check-clk-vs-uboot.py.
#include <Uefi.h>
#include <stdio.h>
#include "RkClockMath.h"

int main (void)
{
  for (unsigned i = 0; i < sizeof (mRkClkPllRates) / sizeof (mRkClkPllRates[0]); i++)
    printf ("pll %u %u %u %u %u\n", mRkClkPllRates[i].Rate, mRkClkPllRates[i].RefDiv, mRkClkPllRates[i].FbDiv,
            mRkClkPllRates[i].PostDiv1, mRkClkPllRates[i].PostDiv2);
  for (unsigned i = 0; i < sizeof (mRkClkCpuRates) / sizeof (mRkClkCpuRates[0]); i++)
    printf ("cpu %u %u %u\n", mRkClkCpuRates[i].Rate, mRkClkCpuRates[i].AclkDiv, mRkClkCpuRates[i].PclkDiv);
  printf ("hz APLL_HZ %u GPLL_HZ %u ACLK_BUS_HZ %u HCLK_BUS_HZ %u PCLK_BUS_HZ %u ACLK_PERI_HZ %u HCLK_PERI_HZ %u\n",
          RKCLK_APLL_HZ, RKCLK_GPLL_HZ, RKCLK_ACLK_BUS_HZ, RKCLK_HCLK_BUS_HZ, RKCLK_PCLK_BUS_HZ, RKCLK_ACLK_PERI_HZ, RKCLK_HCLK_PERI_HZ);
  return 0;
}

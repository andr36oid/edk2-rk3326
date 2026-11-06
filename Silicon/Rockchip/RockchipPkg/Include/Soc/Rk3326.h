/** @file
  RK3326 (PX30) register addresses and the values the boot chain leaves in them.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _SOC_RK3326_H_
#define _SOC_RK3326_H_

#define RK3326_PMUGRF_BASE        0xFF010000
#define RK3326_PMUGRF_OS_REG(n)   (RK3326_PMUGRF_BASE + 0x200 + (n) * 4)
#define RK3326_GRF_BASE           0xFF140000
#define RK3326_SARADC_BASE        0xFF288000
#define RK3326_CRU_BASE           0xFF2B0000
#define RK3326_PMUCRU_BASE        0xFF2BC000
#define RK3326_UART2_BASE         0xFF160000
#define RK3326_SDMMC_BASE         0xFF370000
#define RK3326_EMMC_BASE          0xFF390000
#define RK3326_VOPB_BASE          0xFF460000
#define RK3326_VOPL_BASE          0xFF470000

//
// VOP (lite register layout, both VOPB and VOPL). The boot logo is on WIN1.
//
#define VOP_SYS_CTRL2             0x0018
#define VOP_STANDBY               (1U << 1)
#define VOP_WIN1_CTRL0            0x0090
#define VOP_WIN_EN                (1U << 0)
#define VOP_WIN_FMT(v)            (((v) >> 4) & 0x7)
#define VOP_FMT_ARGB8888          0
#define VOP_FMT_RGB888            1
#define VOP_FMT_RGB565            2
#define VOP_WIN1_VIR              0x0098      // stride in 32-bit words, bits 12:0
#define VOP_WIN1_MST              0x00A0
#define VOP_WIN1_DSP_INFO         0x00A4      // (h-1)<<16 | (w-1)
#define VOP_DSP_HACT_ST_END       0x0104      // start<<16 | end
#define VOP_DSP_VACT_ST_END       0x010C

#endif

/** @file
  The register values U-Boot's board code writes before Linux (odroidgoa.c,
  board.c, the Rockchip io-domain driver). Plain data, so a host test can check
  them against U-Boot's source.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _BOARD_INIT_H_
#define _BOARD_INIT_H_

#include <Uefi.h>

// SYS_GRF (0xFF140000) offsets and PMU_GRF (0xFF010000) offsets.
#define GRF_GPIO1A_IOMUX_L  0x00
#define GRF_GPIO1A_IOMUX_H  0x04
#define GRF_GPIO1B_IOMUX_L  0x08
#define GRF_GPIO1B_IOMUX_H  0x0C
#define GRF_GPIO1A_P        0x60
#define GRF_GPIO1B_P        0x64
#define GRF_GPIO2A_P        0x70
#define GRF_GPIO3B_P        0x84
#define GRF_GPIO1A_E        0xF0
#define GRF_GPIO1B_E        0xF4
#define GRF_IO_VSEL         0x180
#define GRF_IOFUNC_CON0     0x184
#define PMUGRF_IO_VSEL      0x100

// A GRF write: mask in the upper half, value in the lower.
#define GRF_WRITE(Mask, Val)  ((((UINT32)(Mask)) << 16) | ((UINT32)(Val) & (Mask)))

typedef struct {
  UINT16  Offset;
  UINT32  Value;
} GRF_REG_WRITE;

//
// board_init_switch_gpio (odroidgoa.c:144-161): the button pins to GPIO with
// pull-ups. Not the GO3 path, U-Boot's is_odroidgo3() is hard-wired to false.
//
#define SWITCH_GPIO_WRITES                           \
  { GRF_GPIO1A_IOMUX_L, GRF_WRITE (0x0F00, 0) },     \
  { GRF_GPIO1A_IOMUX_H, GRF_WRITE (0xFFF0, 0) },     \
  { GRF_GPIO1B_IOMUX_H, GRF_WRITE (0xFFFF, 0) },     \
  { GRF_GPIO1B_P,       GRF_WRITE (0xFF00, 0x5500) },\
  { GRF_GPIO1A_P,       GRF_WRITE (0xFCC0, 0x5440) },\
  { GRF_GPIO2A_P,       GRF_WRITE (0xFFFF, 0x5555) },\
  { GRF_GPIO3B_P,       GRF_WRITE (0xC030, 0x4010) }

//
// andr36oid_emmc_pins (board.c:396-406): the built-in eMMC's pins. Full 32-bit
// values as U-Boot writes them (the upper half is the mask).
//
#define EMMC_PIN_WRITES                              \
  { GRF_GPIO1A_IOMUX_L, 0xFFFF2222 },                \
  { GRF_GPIO1A_IOMUX_H, 0xFFFF2222 },                \
  { GRF_GPIO1B_IOMUX_L, 0x0FF00220 },                \
  { GRF_GPIO1A_P,       0xFFFF5555 },                \
  { GRF_GPIO1B_P,       0x003C0010 },                \
  { GRF_GPIO1A_E,       0xFFFFAAAA },                \
  { GRF_GPIO1B_E,       0x003C0028 }

//
// IO domain voltage selects (rockchip-io-domain.c). Bit n of VSEL is 0 for a
// 3.3 V supply (above 1.98 V), 1 for 1.8 V. SYS_GRF 0x180: BIT0 is the
// "vccio6 comes from the framework" switch (px30_iodomain_init), bits 1..6 are
// vccio6, vccio1, vccio2, vccio3, vccio4, vccio5. PMU_GRF 0x100: BIT14 pmuio1,
// BIT15 pmuio2. Every screen DTB has its supplies at 3.0 or 3.3 V (vccio_sd
// 1.8..3.3 and boots at 3.3), so all select 3.3 V.
//
#define IO_VSEL_SYS_MASK   0x7FU
#define IO_VSEL_SYS_VALUE  0x01U
#define IO_VSEL_PMU_MASK   0xC000U
#define IO_VSEL_PMU_VALUE  0x0000U

// iofunc_con0 bits 11:10 = 1: UART2 on the M1 pins (board_debug_uart2m1).
#define IOFUNC_UART2_MASK   (3U << 10)
#define IOFUNC_UART2_M1     (1U << 10)

//
// GPIO outputs (odroidgoa.c): bank, pin, level.
//
typedef struct {
  UINT8    Bank;
  UINT8    Pin;
  BOOLEAN  Level;
} BOARD_GPIO_OUT;

#define BOARD_GPIO_RUMBLE   { 0, 15, FALSE }   // GPIO0_B7
#define BOARD_GPIO_PWR_LED  { 0, 18, TRUE  }   // GPIO0_C2
#define BOARD_GPIO_CHG_LED  { 0, 13, FALSE }   // GPIO0_B5, unless the screen enable pin
#define BOARD_GPIO_WIFI_EN  { 3, 14, TRUE  }   // GPIO3_B6

#define BOARD_CHG_LED_PIN   13

typedef struct {
  BOOLEAN       Force;       // reprogram PLLs that are already right
  BOOLEAN       Pins;        // button pins, UART2 mux, GPIO outputs
  BOOLEAN       EmmcPins;    // hand the eMMC its pins
  CONST VOID    *Fdt;        // U-Boot's FDT, NULL if none
} BOARD_INIT_PLAN;

/**
  Run the steps the plan asks for. The clocks and IO domains always run (the
  clock code skips what is already right); Pins and EmmcPins add the rest.
**/
EFI_STATUS
BoardInitRun (
  IN CONST BOARD_INIT_PLAN  *Plan
  );

#endif

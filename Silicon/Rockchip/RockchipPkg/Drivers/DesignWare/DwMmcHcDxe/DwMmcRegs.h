/** @file
  DesignWare MSHC register offsets and bits, as the PX30 controllers have them
  (offsets from the Rockchip kernel dw_mmc.c / U-Boot dwmmc.h). Plain defines
  only, so the host tests can include it.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef DW_MMC_REGS_H_
#define DW_MMC_REGS_H_

#define DW_MMC_CTRL          0x000
#define DW_MMC_PWREN         0x004
#define DW_MMC_CLKDIV        0x008
#define DW_MMC_CLKSRC        0x00c
#define DW_MMC_CLKENA        0x010
#define DW_MMC_TMOUT         0x014
#define DW_MMC_CTYPE         0x018
#define DW_MMC_BLKSIZ        0x01c
#define DW_MMC_BYTCNT        0x020
#define DW_MMC_INTMASK       0x024
#define DW_MMC_CMDARG        0x028
#define DW_MMC_CMD           0x02c
#define DW_MMC_RESP0         0x030
#define DW_MMC_RESP1         0x034
#define DW_MMC_RESP2         0x038
#define DW_MMC_RESP3         0x03c
#define DW_MMC_RINTSTS       0x044
#define DW_MMC_STATUS        0x048
#define DW_MMC_FIFOTH        0x04c
#define DW_MMC_DEBNCE        0x064
#define DW_MMC_VERID         0x06c
#define DW_MMC_HCON          0x070
#define DW_MMC_UHSREG        0x074
#define DW_MMC_BMOD          0x080
#define DW_MMC_PLDMND        0x084
#define DW_MMC_DBADDR        0x088
#define DW_MMC_IDSTS         0x08c
#define DW_MMC_IDINTEN       0x090
#define DW_MMC_FIFO_START_240A  0x200   /* data port, controller version >= 2.40a (PX30) */
#define DW_MMC_FIFO_START_OLD   0x100   /* data port, older controllers */

#define CMD_START_BIT   (1u << 31)

#define MMC_8BIT_MODE  (1 << 16)
#define MMC_4BIT_MODE  (1 << 0)
#define MMC_1BIT_MODE  0

#define DW_MMC_BLOCK_SIZE  512

#define CMD_INDEX_MASK                 0x3F
#define BIT_CMD_RESPONSE_EXPECT        (1 << 6)
#define BIT_CMD_LONG_RESPONSE          (1 << 7)
#define BIT_CMD_CHECK_RESPONSE_CRC     (1 << 8)
#define BIT_CMD_DATA_EXPECTED          (1 << 9)
#define BIT_CMD_READ                   (0 << 10)
#define BIT_CMD_WRITE                  (1 << 10)
#define BIT_CMD_BLOCK_TRANSFER         (0 << 11)
#define BIT_CMD_STREAM_TRANSFER        (1 << 11)
#define BIT_CMD_SEND_AUTO_STOP         (1 << 12)
#define BIT_CMD_WAIT_PRVDATA_COMPLETE  (1 << 13)
#define BIT_CMD_STOP_ABORT_CMD         (1 << 14)
#define BIT_CMD_SEND_INIT              (1 << 15)
#define BIT_CMD_UPDATE_CLOCK_ONLY      (1 << 21)
#define BIT_CMD_READ_CEATA_DEVICE      (1 << 22)
#define BIT_CMD_CCS_EXPECTED           (1 << 23)
#define BIT_CMD_ENABLE_BOOT            (1 << 24)
#define BIT_CMD_EXPECT_BOOT_ACK        (1 << 25)
#define BIT_CMD_DISABLE_BOOT           (1 << 26)
#define BIT_CMD_MANDATORY_BOOT         (0 << 27)
#define BIT_CMD_ALTERNATE_BOOT         (1 << 27)
#define BIT_CMD_VOLT_SWITCH            (1 << 28)
#define BIT_CMD_USE_HOLD_REG           (1 << 29)
#define BIT_CMD_START                  (1u << 31)

#define CMD_INDEX(x)  ((x) & CMD_INDEX_MASK)

#define DW_MMC_INT_EBE       (1 << 15)                          /* End-bit Err */
#define DW_MMC_INT_ACD       (1 << 14)                          /* Auto command done */
#define DW_MMC_INT_SBE       (1 << 13)                          /* Start-bit  Err */
#define DW_MMC_INT_HLE       (1 << 12)                          /* Hardware-lock Err */
#define DW_MMC_INT_FRUN      (1 << 11)                          /* FIFO UN/OV RUN */
#define DW_MMC_INT_HTO       (1 << 10)                          /* Data starvation by host timeout */
#define DW_MMC_INT_DRT       (1 << 9)                           /* Data timeout */
#define DW_MMC_INT_RTO       (1 << 8)                           /* Response timeout */
#define DW_MMC_INT_DCRC      (1 << 7)                           /* Data CRC err */
#define DW_MMC_INT_RCRC      (1 << 6)                           /* Response CRC err */
#define DW_MMC_INT_RXDR      (1 << 5)                           /* Receive FIFO data request */
#define DW_MMC_INT_TXDR      (1 << 4)                           /* Transmit FIFO data request */
#define DW_MMC_INT_DTO       (1 << 3)                           /* Data trans over */
#define DW_MMC_INT_CMD_DONE  (1 << 2)                           /* Command done */
#define DW_MMC_INT_RE        (1 << 1)                           /* Response error */

#define DW_MMC_INT_DATA_ERR  (DW_MMC_INT_EBE  |                   \
                                                 DW_MMC_INT_SBE  |\
                                                 DW_MMC_INT_HLE  |\
                                                 DW_MMC_INT_FRUN |\
                                                 DW_MMC_INT_DCRC |\
                                                 DW_MMC_INT_HTO  |\
                                                 DW_MMC_INT_DRT)

#define DW_MMC_IDMAC_DES0_DIC  (1 << 1)
#define DW_MMC_IDMAC_DES0_LD   (1 << 2)
#define DW_MMC_IDMAC_DES0_FS   (1 << 3)
#define DW_MMC_IDMAC_DES0_CH   (1 << 4)
#define DW_MMC_IDMAC_DES0_ER   (1 << 5)
#define DW_MMC_IDMAC_DES0_CES  (1 << 30)
#define DW_MMC_IDMAC_DES0_OWN  (1u << 31)
#define DW_MMC_IDMAC_DES1_BS1(x)  ((x) & 0x1fff)
#define DW_MMC_IDMAC_DES2_BS2(x)  (((x) & 0x1fff) << 13)

#define DW_MMC_CTRL_RESET       (1 << 0)
#define DW_MMC_CTRL_FIFO_RESET  (1 << 1)
#define DW_MMC_CTRL_DMA_RESET   (1 << 2)
#define DW_MMC_CTRL_INT_EN      (1 << 4)
#define DW_MMC_CTRL_DMA_EN      (1 << 5)
#define DW_MMC_CTRL_IDMAC_EN    (1 << 25)
#define DW_MMC_CTRL_RESET_ALL   (DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET)

#define DW_MMC_STS_DATA_BUSY  (1 << 9)
#define DW_MMC_STS_FIFO_COUNT(x)  (((x) & 0x1fff) << 17)                 /* Number of filled locations in FIFO */
#define GET_STS_FIFO_COUNT(x)     (((x) >> 17) & 0x1fff)
#define DW_MMC_STS_FIFO_FULL(x)   (((x) >> 3) & 1)

#define DW_MMC_BMOD_SWR  (1 << 0)                                /* Software Reset */
#define DW_MMC_BMOD_FB   (1 << 1)                                /* Fix Burst */
#define DW_MMC_BMOD_DE   (1 << 7)                                /* IDMAC Enable */

#define DW_MMC_IDSTS_TI  (1 << 0)                                /* Transmit Interrupt */
#define DW_MMC_IDSTS_RI  (1 << 1)                                /* Receive Interrupt */
#define DW_MMC_IDSTS_FBE  (1 << 2)                               /* Fatal bus error */
#define DW_MMC_IDSTS_DU   (1 << 4)                               /* Descriptor unavailable */
#define DW_MMC_IDSTS_CES  (1 << 5)                               /* Card error summary */
#define DW_MMC_IDSTS_FATAL  (DW_MMC_IDSTS_FBE | DW_MMC_IDSTS_DU | DW_MMC_IDSTS_CES)

/* FIFOTH: burst size code (MSIZE), RX and TX watermarks */
#define DW_MMC_SET_FIFOTH(Msize, Rx, Tx)  ((((Msize) & 0x7) << 28) | (((Rx) & 0xfff) << 16) | ((Tx) & 0xfff))

#define UHS_DDR_MODE  (1 << 16)

#endif

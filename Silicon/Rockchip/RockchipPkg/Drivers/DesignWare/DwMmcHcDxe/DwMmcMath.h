/** @file
  The arithmetic of the DesignWare MSHC driver with no hardware access: card
  clock divider, command register encoding, FIFO thresholds and the IDMAC
  descriptor chain. Kept apart so the host tests in Silicon/Rockchip/Tests/host
  can run it with plain gcc.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef DW_MMC_MATH_H_
#define DW_MMC_MATH_H_

#include <Uefi.h>
#include <Protocol/SdMmcPassThru.h>

#include "DwMmcRegs.h"

//
// One descriptor per 4 KiB: BS1 is 13 bits wide and 4 KiB keeps every piece
// 4-byte aligned whenever the buffer is.
//
#define DW_MMC_IDMAC_CHUNK  4096

//
// The IDMAC descriptor chain is allocated once per controller. 8192 of them
// cover 32 MiB, more than the 0xFFFF blocks the SD and eMMC block drivers
// ever ask for in one command.
//
#define DW_MMC_IDMAC_MAX_DESC  8192

//
// Transfers shorter than this go through the FIFO by hand (SCR, switch
// status): the IDMAC is only worth it for whole blocks.
//
#define DW_MMC_DMA_MIN_BYTES  16

//
// FIFO depth in 32-bit words, from fifo-depth in the PX30 device tree.
//
#define DW_MMC_FIFO_DEPTH  256

typedef struct {
  UINT32    Des0;
  UINT32    Des1;
  UINT32    Des2;
  UINT32    Des3;
} DW_MMC_HC_DMA_DESC_LINE;

typedef enum {
  DwMmcDataNone,
  DwMmcDataRead,
  DwMmcDataWrite
} DW_MMC_DATA_DIR;

/**
  Card clock divider. The card clock is BaseKhz / (2 * Div), or BaseKhz when
  Div is 0. Never above the target; the register is 8 bits wide.

  @param[in]  BaseKhz    Clock the controller gets, in kHz.
  @param[in]  TargetKhz  Clock wanted, in kHz.
  @param[out] ActualKhz  Clock that comes out, in kHz (OPTIONAL).

  @return The CLKDIV register value.
**/
STATIC inline
UINT32
DwMmcCalcClockDiv (
  IN  UINT32  BaseKhz,
  IN  UINT32  TargetKhz,
  OUT UINT32  *ActualKhz  OPTIONAL
  )
{
  UINT32  Div;

  if (TargetKhz >= BaseKhz) {
    Div = 0;
  } else {
    Div = (BaseKhz + (2 * TargetKhz) - 1) / (2 * TargetKhz);
    if (Div > 255) {
      Div = 255;
    }
  }

  if (ActualKhz != NULL) {
    *ActualKhz = (Div == 0) ? BaseKhz : BaseKhz / (2 * Div);
  }

  return Div;
}

/**
  Command register value for one command.

  Auto stop is only for the open-ended multi-block commands (CMD18, CMD25):
  SdDxe and EmmcDxe never send CMD12 themselves, they rely on the host.
  The CMD register's hold flag is always set, as in the kernel driver, and the
  response bits follow the response type. Broadcast commands without a
  response (CMD0) get the init flag and no response.

  @param[in] CommandIndex  Command number.
  @param[in] CommandType   BC, BCR, AC or ADTC.
  @param[in] ResponseType  R1 ... R7.
  @param[in] Dir           Data direction.

  @return The CMD register value without the START bit.
**/
STATIC inline
UINT32
DwMmcBuildCmd (
  IN UINT32           CommandIndex,
  IN UINT32           CommandType,
  IN UINT32           ResponseType,
  IN DW_MMC_DATA_DIR  Dir
  )
{
  UINT32  Cmd;

  Cmd = CMD_INDEX (CommandIndex) | BIT_CMD_USE_HOLD_REG;

  if (CommandType != SdMmcCommandTypeBc) {
    switch (ResponseType) {
      case SdMmcResponseTypeR2:
        Cmd |= BIT_CMD_RESPONSE_EXPECT | BIT_CMD_LONG_RESPONSE |
               BIT_CMD_CHECK_RESPONSE_CRC;
        break;
      case SdMmcResponseTypeR3:
      case SdMmcResponseTypeR4:
        // OCR and fast I/O replies carry no valid CRC
        Cmd |= BIT_CMD_RESPONSE_EXPECT;
        break;
      default:
        Cmd |= BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC;
        break;
    }
  }

  if (CommandIndex == 0) {
    Cmd |= BIT_CMD_SEND_INIT;
  }

  if (CommandIndex == 12) {
    // CMD12 is allowed in the middle of a transfer
    Cmd |= BIT_CMD_STOP_ABORT_CMD;
  }

  if (Dir != DwMmcDataNone) {
    Cmd |= BIT_CMD_DATA_EXPECTED | BIT_CMD_WAIT_PRVDATA_COMPLETE;
    if (Dir == DwMmcDataWrite) {
      Cmd |= BIT_CMD_WRITE;
    }

    if ((CommandIndex == 18) || (CommandIndex == 25)) {
      Cmd |= BIT_CMD_SEND_AUTO_STOP;
    }
  }

  return Cmd;
}

/**
  FIFOTH register value for a transfer, as dw_mci_adjust_fifoth() in the
  kernel: the biggest burst that divides both the block and the second half
  of the FIFO, receive watermark just below it, transmit watermark at half.

  @param[in] BlockSize  Block size in bytes.
  @param[in] UseDma     TRUE for IDMAC, FALSE for FIFO access by hand.

  @return The FIFOTH register value.
**/
STATIC inline
UINT32
DwMmcCalcFifoth (
  IN UINT32   BlockSize,
  IN BOOLEAN  UseDma
  )
{
  STATIC CONST UINT32  Mszs[] = { 1, 4, 8, 16, 32, 64, 128, 256 };
  UINT32               BlkDepth;
  UINT32               TxWmark;
  UINT32               TxInverse;
  UINT32               Msize;
  UINT32               RxWmark;
  INT32                Idx;

  TxWmark   = DW_MMC_FIFO_DEPTH / 2;
  TxInverse = DW_MMC_FIFO_DEPTH - TxWmark;
  Msize     = 0;
  RxWmark   = 1;

  if (UseDma && ((BlockSize % 4) == 0)) {
    BlkDepth = BlockSize / 4;
    for (Idx = 7; Idx > 0; Idx--) {
      if (((BlkDepth % Mszs[Idx]) == 0) && ((TxInverse % Mszs[Idx]) == 0)) {
        Msize   = (UINT32)Idx;
        RxWmark = Mszs[Idx] - 1;
        break;
      }
    }
  } else if (!UseDma) {
    // By hand the FIFO is read when it is half full
    RxWmark = (DW_MMC_FIFO_DEPTH / 2) - 1;
  }

  return DW_MMC_SET_FIFOTH (Msize, RxWmark, TxWmark);
}

/**
  Whether the 32-bit IDMAC can move this buffer in one command: word
  aligned, all below 4 GiB, and it fits in the descriptor chain.

  @param[in] DataPhys   Bus address of the data.
  @param[in] Length     Bytes.

  @retval TRUE   It can.
  @retval FALSE  It can not.
**/
STATIC inline
BOOLEAN
DwMmcDmaReachable (
  IN UINT64  DataPhys,
  IN UINT64  Length
  )
{
  if ((Length == 0) || ((Length & 3) != 0) || ((DataPhys & 3) != 0) ||
      ((DataPhys + Length) > 0x100000000ULL))
  {
    return FALSE;
  }

  return ((Length + DW_MMC_IDMAC_CHUNK - 1) / DW_MMC_IDMAC_CHUNK) <= DW_MMC_IDMAC_MAX_DESC;
}

/**
  Fill a chain of IDMAC descriptors (32-bit, chain mode) for one buffer.

  @param[out] Desc       Descriptor array.
  @param[in]  MaxDesc    Entries in the array.
  @param[in]  DescPhys   Bus address of Desc[0].
  @param[in]  DataPhys   Bus address of the data, below 4 GiB, 4-byte aligned.
  @param[in]  Length     Bytes, a multiple of 4.

  @return Descriptors used, 0 if the buffer does not fit or is not valid.
**/
STATIC inline
UINT32
DwMmcFillDescriptors (
  OUT DW_MMC_HC_DMA_DESC_LINE  *Desc,
  IN  UINT32                   MaxDesc,
  IN  UINT32                   DescPhys,
  IN  UINT64                   DataPhys,
  IN  UINT64                   Length
  )
{
  UINT32  Count;
  UINT32  Index;
  UINT32  Piece;
  UINT64  Left;

  if (!DwMmcDmaReachable (DataPhys, Length)) {
    return 0;
  }

  Count = (UINT32)((Length + DW_MMC_IDMAC_CHUNK - 1) / DW_MMC_IDMAC_CHUNK);
  if (Count > MaxDesc) {
    return 0;
  }

  Left = Length;
  for (Index = 0; Index < Count; Index++) {
    Piece = (Left > DW_MMC_IDMAC_CHUNK) ? DW_MMC_IDMAC_CHUNK : (UINT32)Left;
    Left -= Piece;

    // Owned by the controller, chained, interrupt only on the last one
    Desc[Index].Des0 = DW_MMC_IDMAC_DES0_OWN | DW_MMC_IDMAC_DES0_CH |
                       DW_MMC_IDMAC_DES0_DIC;
    Desc[Index].Des1 = DW_MMC_IDMAC_DES1_BS1 (Piece);
    Desc[Index].Des2 = (UINT32)(DataPhys + (UINT64)Index * DW_MMC_IDMAC_CHUNK);
    Desc[Index].Des3 = DescPhys + (Index + 1) * sizeof (DW_MMC_HC_DMA_DESC_LINE);
  }

  Desc[0].Des0 |= DW_MMC_IDMAC_DES0_FS;

  Desc[Count - 1].Des0 &= ~(UINT32)(DW_MMC_IDMAC_DES0_CH | DW_MMC_IDMAC_DES0_DIC);
  Desc[Count - 1].Des0 |= DW_MMC_IDMAC_DES0_LD;
  Desc[Count - 1].Des3  = 0;

  return Count;
}

#endif

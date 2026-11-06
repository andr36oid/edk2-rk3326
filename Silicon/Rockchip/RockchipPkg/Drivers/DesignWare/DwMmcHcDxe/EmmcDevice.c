/** @file
  This file provides some helper functions which are specific for EMMC device.

  Copyright (c) 2015 - 2021, Intel Corporation. All rights reserved.
  Copyright (c) 2018, Linaro. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <IndustryStandard/Emmc.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "DwMmcHcDxe.h"

#define EMMC_GET_STATE(x)  (((x) >> 9) & 0xf)
#define EMMC_STATE_IDLE   0
#define EMMC_STATE_READY  1
#define EMMC_STATE_IDENT  2
#define EMMC_STATE_STBY   3
#define EMMC_STATE_TRAN   4
#define EMMC_STATE_DATA   5
#define EMMC_STATE_RCV    6
#define EMMC_STATE_PRG    7
#define EMMC_STATE_DIS    8
#define EMMC_STATE_BTST   9
#define EMMC_STATE_SLP    10

#define EMMC_CMD1_CAPACITY_LESS_THAN_2GB     0x00FF8080 // Capacity <= 2GB, byte addressing used
#define EMMC_CMD1_CAPACITY_GREATER_THAN_2GB  0x40FF8080 // Capacity > 2GB, 512-byte sector addressing used

/**
  Send command GO_IDLE_STATE (CMD0 with argument of 0x00000000) to the device to
  make it go to Idle State.

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.4 for details.

  @param[in] PassThru       A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.
  @param[in] Slot           The slot number of the SD card to send the command
                            to.

  @retval EFI_SUCCESS       The EMMC device is reset correctly.
  @retval Others            The device reset fails.

**/
EFI_STATUS
EmmcReset (
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru
  )
{
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;
  EFI_STATUS                           Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = DW_MMC_HC_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex    = EMMC_GO_IDLE_STATE;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeBc;
  SdMmcCmdBlk.ResponseType    = 0;
  SdMmcCmdBlk.CommandArgument = 0;

  gBS->Stall (1000);

  Status = PassThru->PassThru (PassThru, 0, &Packet, NULL);

  return Status;
}

/**
  Send command SEND_OP_COND to the EMMC device to get the data of the OCR
  register.

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.4 for details.

  @param[in]      PassThru  A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.
  @param[in, out] Argument  On input, the argument of SEND_OP_COND is to send
                            to the device.
                            On output, the argument is the value of OCR
                            register.

  @retval EFI_SUCCESS       The operation is done correctly.
  @retval Others            The operation fails.

**/
EFI_STATUS
EmmcGetOcr (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru,
  IN OUT UINT32                         *Argument
  )
{
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;
  EFI_STATUS                           Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = DW_MMC_HC_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex    = EMMC_SEND_OP_COND;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeBcr;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR3;
  SdMmcCmdBlk.CommandArgument = *Argument;

  Status = PassThru->PassThru (PassThru, 0, &Packet, NULL);
  if (!EFI_ERROR (Status)) {
    //
    // For details, refer to SD Host Controller Simplified Spec 3.0 Table 2-12.
    //
    *Argument = SdMmcStatusBlk.Resp0;
  }

  return Status;
}

/**
  Broadcast command ALL_SEND_CID to the bus to ask all the EMMC devices to send
  the data of their CID registers.

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.4 for details.

  @param[in] PassThru       A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.

  @retval EFI_SUCCESS       The operation is done correctly.
  @retval Others            The operation fails.

**/
EFI_STATUS
EmmcGetAllCid (
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru
  )
{
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;
  EFI_STATUS                           Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = DW_MMC_HC_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex    = EMMC_ALL_SEND_CID;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeBcr;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR2;
  SdMmcCmdBlk.CommandArgument = 0;

  Status = PassThru->PassThru (PassThru, 0, &Packet, NULL);

  return Status;
}

/**
  Send command SET_RELATIVE_ADDR to the EMMC device to assign a Relative device
  Address (RCA).

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.4 for details.

  @param[in] PassThru       A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.
  @param[in] Rca            The relative device address to be assigned.

  @retval EFI_SUCCESS       The operation is done correctly.
  @retval Others            The operation fails.

**/
EFI_STATUS
EmmcSetRca (
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru,
  IN UINT16                         Rca
  )
{
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;
  EFI_STATUS                           Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = DW_MMC_HC_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex    = EMMC_SET_RELATIVE_ADDR;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR1;
  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = PassThru->PassThru (PassThru, 0, &Packet, NULL);

  return Status;
}

/**
  Send command SEND_CSD to the EMMC device to get the data of the CSD register.

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.10.4 for details.

  @param[in]  PassThru      A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.
  @param[in]  Rca           The relative device address of selected device.
  @param[out] Csd           The buffer to store the content of the CSD register.
                            Note the caller should ignore the lowest byte of
                            this buffer as the content of this byte is
                            meaningless even if the operation succeeds.

  @retval EFI_SUCCESS       The operation is done correctly.
  @retval Others            The operation fails.

**/
EFI_STATUS
EmmcGetCsd (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru,
  IN     UINT16                         Rca,
  OUT EMMC_CSD                          *Csd
  )
{
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;
  EFI_STATUS                           Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = DW_MMC_HC_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex    = EMMC_SEND_CSD;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR2;
  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = PassThru->PassThru (PassThru, 0, &Packet, NULL);
  if (!EFI_ERROR (Status)) {
    //
    // Copy 128bit data for CSD structure.
    //
    CopyMem ((VOID *)Csd + 1, &SdMmcStatusBlk.Resp0, sizeof (EMMC_CSD) - 1);
  }

  return Status;
}

/**
  Send command SELECT_DESELECT_CARD to the EMMC device to select/deselect it.

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.10.4 for details.

  @param[in]  PassThru      A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.
  @param[in]  Rca           The relative device address of selected device.

  @retval EFI_SUCCESS       The operation is done correctly.
  @retval Others            The operation fails.

**/
EFI_STATUS
EmmcSelect (
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru,
  IN UINT16                         Rca
  )
{
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;
  EFI_STATUS                           Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = DW_MMC_HC_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex    = EMMC_SELECT_DESELECT_CARD;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR1;
  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = PassThru->PassThru (PassThru, 0, &Packet, NULL);

  return Status;
}

/**
  Send command SEND_EXT_CSD to the EMMC device to get the data of the EXT_CSD
  register.

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.10.4 for details.

  @param[in]  PassThru      A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.
  @param[out] ExtCsd        The buffer to store the content of the EXT_CSD
                            register.

  @retval EFI_SUCCESS       The operation is done correctly.
  @retval Others            The operation fails.

**/
EFI_STATUS
EmmcGetExtCsd (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru,
  OUT EMMC_EXT_CSD                      *ExtCsd
  )
{
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;
  EFI_STATUS                           Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = DW_MMC_HC_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex    = EMMC_SEND_EXT_CSD;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeAdtc;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR1;
  SdMmcCmdBlk.CommandArgument = 0x00000000;

  Packet.InDataBuffer     = ExtCsd;
  Packet.InTransferLength = sizeof (EMMC_EXT_CSD);

  Status = PassThru->PassThru (PassThru, 0, &Packet, NULL);
  return Status;
}

/**
  Send command SWITCH to the EMMC device to switch the mode of operation of the
  selected Device or modifies the EXT_CSD registers.

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.10.4 for details.

  @param[in]  PassThru      A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.
  @param[in]  Access        The access mode of SWTICH command.
  @param[in]  Index         The offset of the field to be access.
  @param[in]  Value         The value to be set to the specified field of
                            EXT_CSD register.
  @param[in]  CmdSet        The value of CmdSet field of EXT_CSD register.

  @retval EFI_SUCCESS       The operation is done correctly.
  @retval Others            The operation fails.

**/
EFI_STATUS
EmmcSwitch (
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru,
  IN UINT8                          Access,
  IN UINT8                          Index,
  IN UINT8                          Value,
  IN UINT8                          CmdSet
  )
{
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;
  EFI_STATUS                           Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = DW_MMC_HC_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex    = EMMC_SWITCH;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR1b;
  SdMmcCmdBlk.CommandArgument = (Access << 24) | (Index << 16) | \
                                (Value << 8) | CmdSet;

  Status = PassThru->PassThru (PassThru, 0, &Packet, NULL);

  return Status;
}

/**
  Send command SEND_STATUS to the addressed EMMC device to get its status
  register.

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.10.4 for details.

  @param[in]  PassThru      A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.
  @param[in]  Rca           The relative device address of addressed device.
  @param[out] DevStatus     The returned device status.

  @retval EFI_SUCCESS       The operation is done correctly.
  @retval Others            The operation fails.

**/
EFI_STATUS
EmmcSendStatus (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru,
  IN     UINT16                         Rca,
  OUT UINT32                            *DevStatus
  )
{
  EFI_SD_MMC_COMMAND_BLOCK             SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK              SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  Packet;
  EFI_STATUS                           Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;
  Packet.Timeout        = DW_MMC_HC_GENERIC_TIMEOUT;

  SdMmcCmdBlk.CommandIndex    = EMMC_SEND_STATUS;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR1;
  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = PassThru->PassThru (PassThru, 0, &Packet, NULL);
  if (!EFI_ERROR (Status)) {
    *DevStatus = SdMmcStatusBlk.Resp0;
  }

  return Status;
}

/**
  Poll CMD13 until the card is out of programming state, which is how a
  switch (CMD6, R1b) is known to be done, and optionally until it is in
  transfer state. Gives up after a second.

  @param[in]  PassThru      The pass thru instance.
  @param[in]  Rca           The relative device address.
  @param[in]  WantTran      Wait for transfer state, not just "not busy".
  @param[out] DevStatus     The last status word.

  @retval EFI_SUCCESS       Ready.
  @retval EFI_TIMEOUT       Still busy after a second.
  @retval Others            CMD13 failed.

**/
STATIC
EFI_STATUS
EmmcWaitReady (
  IN  EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru,
  IN  UINT16                         Rca,
  IN  BOOLEAN                        WantTran,
  OUT UINT32                         *DevStatus
  )
{
  EFI_STATUS  Status;
  UINT32      Try;
  UINT32      State;

  for (Try = 0; Try < 1000; Try++) {
    Status = EmmcSendStatus (PassThru, Rca, DevStatus);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    State = EMMC_GET_STATE (*DevStatus);
    if (WantTran ? (State == EMMC_STATE_TRAN) : (State != EMMC_STATE_PRG)) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1000);
  }

  return EFI_TIMEOUT;
}

/**
  Switch the bus width to specified width.

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.6.9.

  @param[in] DevIo          A pointer to the EFI_DEVICE_IO_PROTOCOL instance.
  @param[in] PassThru       A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.
  @param[in] Rca            The relative device address to be assigned.
  @param[in] IsDdr          If TRUE, use dual data rate data simpling method.
                            Otherwise use single data rate data simpling method.
  @param[in] BusWidth       The bus width to be set, it could be 4 or 8.

  @retval EFI_SUCCESS       The operation is done correctly.
  @retval Others            The operation fails.

**/
EFI_STATUS
EmmcSwitchBusWidth (
  IN UINTN                          DevBase,
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru,
  IN UINT16                         Rca,
  IN BOOLEAN                        IsDdr,
  IN UINT8                          BusWidth
  )
{
  EFI_STATUS  Status;
  UINT8       Access;
  UINT8       Index;
  UINT8       Value;
  UINT8       CmdSet;
  UINT32      DevStatus;

  //
  // Write Byte, the Value field is written into the byte pointed by Index.
  //
  Access = 0x03;
  Index  = OFFSET_OF (EMMC_EXT_CSD, BusWidth);
  if (BusWidth == 1) {
    Value = 0;
  } else {
    if (BusWidth == 4) {
      Value = 1;
    } else if (BusWidth == 8) {
      Value = 2;
    } else {
      return EFI_INVALID_PARAMETER;
    }

    if (IsDdr) {
      Value += 4;
    }
  }

  CmdSet = 0;
  Status = EmmcSwitch (PassThru, Access, Index, Value, CmdSet);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "EmmcSwitchBusWidth: Switch to bus width %d fails with %r\n",
      BusWidth,
      Status
      ));
    return Status;
  }

  Status = EmmcWaitReady (PassThru, Rca, FALSE, &DevStatus);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "EmmcSwitchBusWidth: Send status fails with %r\n", Status));
    return Status;
  }

  if ((DevStatus & BIT7) != 0) {
    DEBUG ((DEBUG_ERROR, "EmmcSwitchBusWidth: the card refused bus width %d, status 0x%08x\n", BusWidth, DevStatus));
    return EFI_DEVICE_ERROR;
  }

  Status = DwMmcHcSetBusWidth (DevBase, IsDdr, BusWidth);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Status;
}

/**
  Switch the clock frequency to the specified value.

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.6.

  @param[in] DevIo          A pointer to the EFI_DEVICE_IO_PROTOCOL instance.
  @param[in] PassThru       A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.
  @param[in] Rca            The relative device address to be assigned.
  @param[in] HsTiming       The value to be written to HS_TIMING field of
                            EXT_CSD register.
  @param[in] ClockFreq      The max clock frequency to be set, the unit is MHz.

  @retval EFI_SUCCESS       The operation is done correctly.
  @retval Others            The operation fails.

**/
EFI_STATUS
EmmcSwitchClockFreq (
  IN UINTN                          DevBase,
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru,
  IN UINT16                         Rca,
  IN UINT8                          HsTiming,
  IN UINT32                         ClockFreq
  )
{
  EFI_STATUS              Status;
  UINT8                   Access;
  UINT8                   Index;
  UINT8                   Value;
  UINT8                   CmdSet;
  UINT32                  DevStatus;
  DW_MMC_HC_PRIVATE_DATA  *Private;

  Private = DW_MMC_HC_PRIVATE_FROM_THIS (PassThru);
  //
  // Write Byte, the Value field is written into the byte pointed by Index.
  //
  Access = 0x03;
  Index  = OFFSET_OF (EMMC_EXT_CSD, HsTiming);
  Value  = HsTiming;
  CmdSet = 0;

  Status = EmmcSwitch (PassThru, Access, Index, Value, CmdSet);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "EmmcSwitchClockFreq: Switch to hstiming %d fails with %r\n",
      HsTiming,
      Status
      ));
    return Status;
  }

  Status = EmmcWaitReady (PassThru, Rca, FALSE, &DevStatus);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "EmmcSwitchClockFreq: Send status fails with %r\n",
      Status
      ));
    return Status;
  }

  //
  // Check the switch operation is really successful or not.
  //
  if ((DevStatus & BIT7) != 0) {
    DEBUG ((
      DEBUG_ERROR,
      "EmmcSwitchClockFreq: The switch operation fails as DevStatus 0x%08x\n",
      DevStatus
      ));
    return EFI_DEVICE_ERROR;
  }

  //
  // Convert the clock freq unit from MHz to KHz.
  //
  Status = DwMmcHcClockSupply (DevBase, ClockFreq * 1000, Private->Capability[0]);

  return Status;
}

/**
  Set the card up for data transfer: select it, read EXT_CSD, switch to high
  speed timing and the widest bus the slot has. No HS200 or DDR: those need
  I/O voltage switching and tuning, which these boards don't have.
  Refer to EMMC Electrical Standard Spec 5.1 Section 6.6.8.

  @param[in] DevBase        Controller registers.
  @param[in] PassThru       A pointer to the EFI_SD_MMC_PASS_THRU_PROTOCOL
                            instance.
  @param[in] Rca            The relative device address to be assigned.

  @retval EFI_SUCCESS       The operation is done correctly.
  @retval Others            The operation fails.

**/
EFI_STATUS
EmmcSetBusMode (
  IN UINTN                          DevBase,
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru,
  IN UINT16                         Rca
  )
{
  EFI_STATUS              Status;
  EMMC_CSD                Csd;
  EMMC_EXT_CSD            ExtCsd;
  UINT32                  DevStatus;
  UINT32                  ClockFreq;
  UINT8                   BusWidth;
  DW_MMC_HC_PRIVATE_DATA  *Private;

  Private = DW_MMC_HC_PRIVATE_FROM_THIS (PassThru);

  ASSERT (Private->Capability[0].BaseClkFreq != 0);

  Status = EmmcGetCsd (PassThru, Rca, &Csd);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "EmmcSetBusMode: GetCsd fails with %r\n", Status));
    return Status;
  }

  Status = EmmcSelect (PassThru, Rca);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "EmmcSetBusMode: Select fails with %r\n", Status));
    return Status;
  }

  Status = EmmcWaitReady (PassThru, Rca, TRUE, &DevStatus);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "EmmcSetBusMode: card does not reach transfer state: %r\n", Status));
    return Status;
  }

  Status = EmmcGetExtCsd (PassThru, &ExtCsd);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "EmmcSetBusMode: GetExtCsd fails with %r\n", Status));
    return Status;
  }

  BusWidth = (UINT8)Private->Capability[0].BusWidth;

  if (((ExtCsd.DeviceType & BIT1) != 0) && (Private->Capability[0].HighSpeed != 0)) {
    ClockFreq = 52;
  } else if (((ExtCsd.DeviceType & BIT0) != 0) && (Private->Capability[0].HighSpeed != 0)) {
    ClockFreq = 26;
  } else {
    // Card without high speed: the backward compatible timing allows 20 MHz
    ClockFreq = 0;
  }

  DEBUG ((
    DEBUG_INFO,
    "EmmcSetBusMode: DeviceType 0x%x, %d MHz, %d-bit\n",
    ExtCsd.DeviceType,
    ClockFreq != 0 ? ClockFreq : 20,
    BusWidth
    ));

  if (ClockFreq != 0) {
    Status = EmmcSwitchClockFreq (DevBase, PassThru, Rca, 1, ClockFreq);
  } else {
    Status = DwMmcHcClockSupply (DevBase, 20000, Private->Capability[0]);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EmmcSwitchBusWidth (DevBase, PassThru, Rca, FALSE, BusWidth);
}

/**
  Execute EMMC device identification procedure.

  Refer to EMMC Electrical Standard Spec 5.1 Section 6.4 for details.

  @param[in] Private        A pointer to the DW_MMC_HC_PRIVATE_DATA instance.

  @retval EFI_SUCCESS       There is a EMMC card.
  @retval Others            There is not a EMMC card.

**/
EFI_STATUS
EmmcIdentification (
  IN DW_MMC_HC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS                     Status;
  UINTN                          DevBase;
  EFI_SD_MMC_PASS_THRU_PROTOCOL  *PassThru;
  UINT32                         Ocr;
  UINT16                         Rca;
  UINT32                         DevStatus;
  UINT32                         Timeout;

  DevBase  = Private->DevBase;
  PassThru = &Private->PassThru;

  Status = EmmcReset (PassThru);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "EmmcIdentification: Executing Cmd0 fails with %r\n",
      Status
      ));
    return Status;
  }

  // Up to a second for the card to finish its power up
  Timeout = 1000;
  do {
    Ocr    = EMMC_CMD1_CAPACITY_GREATER_THAN_2GB;
    Status = EmmcGetOcr (PassThru, &Ocr);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_INFO,
        "EmmcIdentification: Executing Cmd1 fails with %r\n",
        Status
        ));
      return Status;
    }

    if (--Timeout <= 0) {
      return EFI_DEVICE_ERROR;
    }

    MicroSecondDelay (1000);
  } while ((Ocr & BIT31) == 0);

  Status = EmmcGetAllCid (PassThru);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "EmmcIdentification: Executing Cmd2 fails with %r\n",
      Status
      ));
    return Status;
  }

  //
  // valid RCA starts from 1.
  // Here we takes a simple formula to calculate the RCA.
  // Don't support multiple devices on the slot, that is
  // shared bus slot feature.
  //
  Rca    = 1;
  Status = EmmcSetRca (PassThru, Rca);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "EmmcIdentification: Executing Cmd3 fails with %r\n",
      Status
      ));
    return Status;
  }

  //
  // Enter Data Tranfer Mode.
  //
  DEBUG ((
    DEBUG_INFO,
    "EmmcIdentification: Found a EMMC device at RCA [%d]\n",
    Rca
    ));
  Private->Slot[0].CardType = EmmcCardType;

  Status = EmmcSetBusMode (DevBase, PassThru, Rca);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The card has to be ready for the first data command.
  //
  Status = EmmcWaitReady (PassThru, Rca, TRUE, &DevStatus);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "EmmcIdentification: card not in transfer state: %r\n", Status));
  }

  return Status;
}

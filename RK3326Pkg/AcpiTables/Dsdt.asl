/** @file
*  Differentiated System Description Table Fields (DSDT) for the GameConsole R50S.
*
*  Copyright (c) 2025, Efim Gîrjeu <efimgirjeu@gmail.com>
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <IndustryStandard/Acpi60.h>

DefinitionBlock ("DsdtTable.aml", "DSDT",
                 EFI_ACPI_6_0_DIFFERENTIATED_SYSTEM_DESCRIPTION_TABLE_REVISION,
                 "RKCP  ", "RK3326  ", FixedPcdGet32 (PcdAcpiDefaultOemRevision)) {
  Scope (_SB) {

    include ("Cpu.asl")
    // include ("Tsadc.asl")
    include ("Uart.asl")
    include ("Wdt.asl")
    // include ("Usb2.asl")
    include ("Mshc.asl")

  } // Scope (_SB)
}

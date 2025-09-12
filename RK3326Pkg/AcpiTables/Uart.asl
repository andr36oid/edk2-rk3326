/** @file
*  UART devices.
*
*  Copyright (c) 2022, Jared McNeill <jmcneill@invisible.ca>
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <IndustryStandard/Acpi60.h>

// UART1 DW APB
Device (URT1) {
    Name (_HID, "HISI0031")
    Name (_CID, "8250dw");
    Name (_UID, 2)
    Name (_STA, 0xF)
    Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xFF158000, 0x0001000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 51 }
    })
    Name (_DSD, Package () {
        ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package () {
            Package () { "clock-frequency", 24000000 },
            Package () { "reg-shift", 2 },
            Package () { "reg-io-width", 4 },
        }
    })
}

// UART2 DW APB
Device (URT2) {
    Name (_HID, "HISI0031")
    Name (_CID, "8250dw");
    Name (_UID, 3)
    Name (_STA, 0xF)
    Name (_CRS, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xFF160000, 0x0001000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 52 }
    })
    Name (_DSD, Package () {
        ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package () {
            Package () { "clock-frequency", 24000000 },
            Package () { "reg-shift", 2 },
            Package () { "reg-io-width", 4 },
        }
    })
}
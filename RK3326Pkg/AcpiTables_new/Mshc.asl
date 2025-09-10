/** @file
*  DesignWare Mobile Storage Host Controller (SD/SDIO) devices.
*
*  Copyright (c) 2025, Efim Gîrjeu <efimgirjeu@gmail.com>
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <IndustryStandard/Acpi60.h>

// Mobile Storage Host Controller
Device (SDC0) {
    Name (_HID, "PNP0D40")
    Name (_CID, "PNP0D40")
    Name (_UID, 0)

    Method (_CRS, 0x0, Serialized) {
        Name (RBUF, ResourceTemplate() {
            Memory32Fixed (ReadWrite, 0xFF370000, 0x4000)
            Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 86 }
        })
        Return (RBUF)
    }
}
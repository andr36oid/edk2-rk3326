/** @file
*  DesignWare Mobile Storage Host Controller (SD/SDIO) devices.
*
*  Copyright (c) 2025, Efim Gîrjeu <efimgirjeu@gmail.com>
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <IndustryStandard/Acpi60.h>

// Mobile Storage Host Controller
Device (MSH0) {
    Name (_HID, "PRP0001")
    Name (_UID, 4)
    Name (_CCA, Zero)

    Name (_DSD, Package () {
        ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package () {
            Package () { "compatible", Package () { "rockchip,px30-dw-mshc", "rockchip,rk3288-dw-mshc" } },
            Package () { "fifo-depth", 0x100 },
            Package () { "max-frequency", 50000000 },
            Package () { "bus-width", 4 },
            Package () { "cap-sd-highspeed", 1 },
            Package () { "cap-mmc-highspeed", 1 },
            Package () { "supports-sd", 1 },
            Package () { "disable-wp", 1 },
        }
    })

    Method (_CRS, 0x0, Serialized) {
        Name (RBUF, ResourceTemplate() {
            Memory32Fixed (ReadWrite, 0xFF370000, 0x4000)
            Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 86 }
        })
        Return (RBUF)
    }
}
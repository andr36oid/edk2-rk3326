/** @file
 *
 *  Copyright (c) 2024, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include "AcpiHeader.h"

Scope (\_SB_) {
  Name (SDRM, 1) // SD slot is removable

  Device (SDHC) {
    Name (_HID, "SDHC0002")
    Name (_UID, 0x0)
    Name (_CCA, 0x0)
    Name (_S1D, 0x1)
    Name (_S2D, 0x1)
    Name (_S3D, 0x1)
    Name (_S4D, 0x1)
    Name (_STA, 0xf)

    Method (_CRS, 0x0, Serialized) {
      Name (RBUF, ResourceTemplate () {
        Memory32Fixed (ReadWrite, 0xff370000, 0x4000)
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 86 }

        // SDMMC_DET
        GpioIO (Shared, PullUp, 0, 0, IoRestrictionNone, "\\_SB.GPI0") { GPIO_PIN_PA3 }
        GpioInt (Edge, ActiveBoth, Shared, PullUp, 0, "\\_SB.GPI0") { GPIO_PIN_PA3 }
      })
      Return (RBUF)
    }

    Name (_DSD, Package () {
      ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
      Package () {
        Package () { "compatible", Package () { "rockchip,px30-dw-mshc", "rockchip,rk3288-dw-mshc" } },
        Package () { "fifo-depth", 0x100 },
        Package () { "max-frequency", 150000000 },
        Package () { "bus-width", 4 },
        Package () { "cap-sd-highspeed", 1 },
        Package () { "sd-uhs-sdr12", 1},
        Package () { "sd-uhs-sdr25", 1},
        Package () { "sd-uhs-sdr50", 1 },
        Package () { "sd-uhs-sdr104", 1 },
      }
    })

    //
    // A child device that represents the SD card.
    //
    Device (SDMM) {
      Name (_ADR, 0x0)

      Method (_RMV) {
        Return (SDRM)
      }
    }
  }
}

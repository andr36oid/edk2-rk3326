/** @file
 *
 *  Copyright (c) 2022, Rockchip Electronics Co. Ltd.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/
#include "AcpiHeader.h"

//
// Description: GPIO
//
Device (GPI0)
{
  Name (_HID, "RKCP3002")
  Name (_CID, "PRP0001")
  Name (_UID, 0x0)
  Name (_DSD, Package () {
    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
        Package () { "compatible", "rockchip,gpio-bank" },
    }
  })
  Method (_CRS, 0x0, NotSerialized)
  {
    Name (RBUF, ResourceTemplate()
    {
      Memory32Fixed(ReadWrite, 0xFF040000, 0x100)
      Interrupt(ResourceConsumer, Level, ActiveHigh, Shared) {35}
    })
    Return(RBUF)
  }
  Method (_STA)
  {
    Return(0xf)
  }
}//GPIO0


Device (GPI1)
{
  Name (_HID, "RKCP3002")
  Name (_CID, "PRP0001")
  Name (_UID, 0x1)
  Name (_DSD, Package () {
    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
        Package () { "compatible", "rockchip,gpio-bank" },
    }
  })
  Method (_CRS, 0x0, NotSerialized)
  {
    Name (RBUF, ResourceTemplate()
    {
      Memory32Fixed(ReadWrite, 0xFF250000, 0x100)
      Interrupt(ResourceConsumer, Level, ActiveHigh, Shared) {36}
    })
    Return(RBUF)
  }
  Method (_STA)
  {
    Return(0xf)
  }
}//GPIO1

Device (GPI2)
{
  Name (_HID, "RKCP3002")
  Name (_CID, "PRP0001")
  Name (_UID, 0x2)
  Name (_DSD, Package () {
    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
        Package () { "compatible", "rockchip,gpio-bank" },
    }
  })
  Method (_CRS, 0x0, NotSerialized)
  {
    Name (RBUF, ResourceTemplate()
    {
      Memory32Fixed(ReadWrite, 0xFF260000, 0x100)
      Interrupt(ResourceConsumer, Level, ActiveHigh, Shared) {37}
    })
    Return(RBUF)
  }
  Method (_STA)
  {
    Return(0xf)
  }
}//GPIO2

Device (GPI3)
{
  Name (_HID, "RKCP3002")
  Name (_CID, "PRP0001")
  Name (_UID, 0x3)
  Name (_DSD, Package () {
    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
        Package () { "compatible", "rockchip,gpio-bank" },
    }
  })
  Method (_CRS, 0x0, NotSerialized)
  {
    Name (RBUF, ResourceTemplate()
    {
      Memory32Fixed(ReadWrite, 0xFF270000, 0x100)
      Interrupt(ResourceConsumer, Level, ActiveHigh, Shared) {38}
    })
    Return(RBUF)
  }
  Method (_STA)
  {
    Return(0xf)
  }
}//GPIO3

Device (PINC)
{
  Name (_HID, "PRP0001")
  Name (_UID, 0x4)
  Name (_DSD, Package () {
    ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
        Package () { "compatible", "rockchip,px30-pinctrl" },
    }
  })
  Method (_CRS, 0x0, NotSerialized)
  {
    Name (RBUF, ResourceTemplate()
    {
      Memory32Fixed(ReadWrite, 0xFF140000, 0x1000)
    })
    Return(RBUF)
  }
  Method (_STA)
  {
    Return(0xf)
  }
}

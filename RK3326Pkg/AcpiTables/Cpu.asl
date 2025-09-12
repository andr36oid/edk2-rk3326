/** @file
*  CPU devices.
*
*  Copyright (c) 2025, Efim Gîrjeu <efimgirjeu@gmail.com>
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <IndustryStandard/Acpi60.h>

Device (CPU0) {
    Name (_HID, "ACPI0007" /* Processor Device */)  // _HID: Hardware ID
    Name (_UID, 0)  // _UID: Unique ID
	Method (_STA)
     {
         Return(0xf)
     }
}

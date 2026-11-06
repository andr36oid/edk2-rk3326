/** @file
  What to do with the USB-C port's 5 V, as a pure function so it can be tested
  on the host.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef USB_VBUS_POLICY_H_
#define USB_VBUS_POLICY_H_

typedef enum {
  UsbVbusHost,         // switch the host supply on
  UsbVbusCharge        // leave the port charging: host supply off
} USB_VBUS_DECISION;

typedef struct {
  BOOLEAN    SharedWithCharger;   // DT: andr36oid,boost-only-for-otg or andr36oid,usb-host-supply
  BOOLEAN    ChargerPresent;      // DC detect pin or the PMIC says power is in
} USB_VBUS_INPUT;

STATIC
inline
USB_VBUS_DECISION
UsbVbusDecide (
  IN CONST USB_VBUS_INPUT  *In
  )
{
  //
  // On a board where host mode cuts the charge input (D-28S: one port, one
  // switch), a charger that is plugged in wins: the console would stop
  // charging and not even see the charger. Everywhere else host power is
  // free to turn on, a charger or not.
  //
  if (In->SharedWithCharger && In->ChargerPresent) {
    return UsbVbusCharge;
  }

  return UsbVbusHost;
}

#endif

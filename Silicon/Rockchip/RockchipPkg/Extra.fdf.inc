##
#  Rockchip SoC family: drivers every Rockchip device carries.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

  # SD card / eMMC (Synopsys DesignWare MSHC)
  INF RockchipPkg/Drivers/DesignWare/DwMmcHcDxe/DwMmcHcDxe.inf
  INF RockchipPkg/Drivers/DesignWare/RkSdmmcDxe/RkSdmmcDxe.inf

  # USB host on the USB-C port (DWC2), keyboard, mouse, storage
  INF RockchipPkg/Drivers/DwUsbHostDxe/DwUsbHostDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

!include SiliciumPkg/Extra.fdf.inc

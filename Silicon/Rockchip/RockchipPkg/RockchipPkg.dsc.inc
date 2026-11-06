##
#  Rockchip SoC family: shared build settings. Included by the SoC .dsc.inc.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

!include SiliciumPkg/SiliciumPkg.dsc.inc

[PcdsFixedAtBuild]
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemId|"RKCP  "

#
# Our drivers log their steps at DEBUG_INFO, which SiliciumPkg's DEBUG mask
# (0x8007EE0F) drops; they get it back one by one so the Mu core (DxeCore's
# InstallProtocolInterface lines and so on) stays quiet. RELEASE is unaffected:
# its fixed level (Andr36oidPkg.dsc.inc) still lets only ERROR, WARN and INIT out.
#
[Components]
  RockchipPkg/Drivers/DesignWare/DwMmcHcDxe/DwMmcHcDxe.inf {
    <PcdsFixedAtBuild>
      gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8007EE4F
  }
  RockchipPkg/Drivers/DesignWare/RkSdmmcDxe/RkSdmmcDxe.inf {
    <PcdsFixedAtBuild>
      gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8007EE4F
  }
  RockchipPkg/Drivers/DwUsbHostDxe/DwUsbHostDxe.inf {
    <PcdsFixedAtBuild>
      gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8007EE4F
  }
  MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf
  RockchipPkg/Drivers/RkFbDxe/RkFbDxe.inf {
    <PcdsFixedAtBuild>
      gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8007EE4F
  }
  RockchipPkg/Drivers/RkDisplayDxe/RkDisplayDxe.inf {
    <PcdsFixedAtBuild>
      gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8007EE4F
  }

  # Interface stubs, built here so they keep compiling
  RockchipPkg/Library/RkPmicLibNull/RkPmicLibNull.inf

[LibraryClasses.common.DXE_DRIVER]
  RkGopLib|RockchipPkg/Library/RkGopLib/RkGopLib.inf

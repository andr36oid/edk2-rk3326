##
#  Rockchip SoC family: shared build settings. Included by the SoC .dsc.inc.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

!include SiliciumPkg/SiliciumPkg.dsc.inc

[PcdsFixedAtBuild]
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemId|"RKCP  "

[Components]
  RockchipPkg/Drivers/DesignWare/DwMmcHcDxe/DwMmcHcDxe.inf
  RockchipPkg/Drivers/DesignWare/RkSdmmcDxe/RkSdmmcDxe.inf
  RockchipPkg/Drivers/RkFbDxe/RkFbDxe.inf

  # Interface stubs, built here so they keep compiling
  RockchipPkg/Library/RkPmicLibNull/RkPmicLibNull.inf

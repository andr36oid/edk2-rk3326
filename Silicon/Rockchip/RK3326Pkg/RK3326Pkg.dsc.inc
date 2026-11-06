##
#  RK3326 / PX30: 4x Cortex-A35, GIC-400.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  USE_PHYSICAL_TIMER             = 1

!include RockchipPkg/RockchipPkg.dsc.inc

[BuildOptions]
  *_*_AARCH64_CC_FLAGS = -march=armv8-a+crc -mtune=cortex-a35

[PcdsFixedAtBuild]
  gArmPlatformTokenSpaceGuid.PcdCoreCount|4
  gArmPlatformTokenSpaceGuid.PcdClusterCount|1

  gArmTokenSpaceGuid.PcdGicDistributorBase|0xFF131000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0xFF132000

  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x2020363233334B52
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x00003326

  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Rockchip RK3326 @ 1.30 GHz"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"RK3326"

  gRockchipPkgTokenSpaceGuid.PcdRkSdmmcBaseAddress|0xFF370000

[LibraryClasses]
  PlatformSecLib|RK3326Pkg/Library/PlatformSecLib/PlatformSecLib.inf
  PlatformPeiLib|RK3326Pkg/Library/PlatformPeiLib/PlatformPeiLib.inf
  RkPlatformInfoLib|RK3326Pkg/Library/RkPlatformInfoLib/RkPlatformInfoLib.inf

  # Log to RAM (saved to the card by the boot app), the screen and optionally UART2
  SerialPortLib|RockchipPkg/Library/RkSerialPortLib/RkSerialPortLib.inf
  RkLogLib|RockchipPkg/Library/RkSerialPortLib/RkSerialPortLib.inf

  # Board bring-up from C: I2C, the RK817 on it, SARADC
  RkI2cLib|RK3326Pkg/Library/RkI2cLib/RkI2cLib.inf
  RkPmicLib|RockchipPkg/Library/RkPmicLibRk817/RkPmicLibRk817.inf
  RkSaradcLib|RK3326Pkg/Library/RkSaradcLib/RkSaradcLib.inf

  # PSCI resets, power off through the RK817. The Hw class is what
  # ResetSystemRuntimeDxe links (the build warns it is not a declared class of
  # the instance, same as for the PSCI one it replaces).
  HwResetSystemLib|RK3326Pkg/Library/ResetSystemLib/ResetSystemLib.inf
  ResetSystemLib|RK3326Pkg/Library/ResetSystemLib/ResetSystemLib.inf
  GpioLib|RK3326Pkg/Library/GpioLib/GpioLib.inf
  CruLib|RK3326Pkg/Library/CruLib/CruLib.inf
  RkSdmmcPlatformLib|RK3326Pkg/Library/RkSdmmcPlatformLib/RkSdmmcPlatformLib.inf

  # The DesignWare MMC controllers are not cache coherent.
  DmaLib|EmbeddedPkg/Library/NonCoherentDmaLib/NonCoherentDmaLib.inf

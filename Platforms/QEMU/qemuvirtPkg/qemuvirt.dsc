##
#  QEMU "virt" test device for the andr36oid boot app (tools/qemu-test.sh):
#  the same Andr36oidPkg code as the handhelds on a PL011, GICv2 and RAM at
#  1 GB, without any Rockchip driver. BootKeysLib and RkPmicLib stay null.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME                  = qemuvirt
  PLATFORM_GUID                  = 5A0E7C3D-91B4-4E26-8F5A-2C6D0B9E1734
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/qemuvirtPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = qemuvirtPkg/qemuvirt.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 1
  USE_PHYSICAL_TIMER             = 1

!include SiliciumPkg/SiliciumPkg.dsc.inc

[BuildOptions]
  *_*_AARCH64_CC_FLAGS = -march=armv8-a+crc -mtune=cortex-a35

[PcdsFixedAtBuild]
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000

  # Right after the FD (base/size in Resources/Configs/qemuvirt.toml)
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x42300000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  gArmPlatformTokenSpaceGuid.PcdCoreCount|1
  gArmPlatformTokenSpaceGuid.PcdClusterCount|1

  gArmTokenSpaceGuid.PcdGicDistributorBase|0x08000000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0x08010000

  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemId|"QEMU  "
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemTableId|0x5452495655554D45
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x00000001

  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"andr36oid"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"QEMU virt test"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"qemuvirt"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"qemuvirt"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"virt"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Cortex-A35 (QEMU)"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"virt"

  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|640
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|480
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

[PcdsFeatureFlag]
  gAndr36oidPkgTokenSpaceGuid.PcdBootSerialConsole|TRUE

[LibraryClasses]
  MemoryMapLib|qemuvirtPkg/Library/MemoryMapLib/MemoryMapLib.inf
  PlatformSecLib|qemuvirtPkg/Library/PlatformSecLib/PlatformSecLib.inf
  PlatformPeiLib|qemuvirtPkg/Library/PlatformPeiLib/PlatformPeiLib.inf
  QemuVirtInfoLib|qemuvirtPkg/Library/QemuVirtInfoLib/QemuVirtInfoLib.inf
  # PL011 + the same RAM log ring the handhelds have
  SerialPortLib|qemuvirtPkg/Library/QemuSerialPortLib/QemuSerialPortLib.inf
  RkLogLib|qemuvirtPkg/Library/QemuSerialPortLib/QemuSerialPortLib.inf
  # No buttons, no PMIC: the null instances
  BootKeysLib|Andr36oidPkg/Library/BootKeysLibNull/BootKeysLibNull.inf
  RkPmicLib|RockchipPkg/Library/RkPmicLibNull/RkPmicLibNull.inf
  BootPlatformLib|qemuvirtPkg/Library/BootPlatformLib/BootPlatformLib.inf

[Components]
  qemuvirtPkg/Drivers/MemDiskDxe/MemDiskDxe.inf

!include Andr36oidPkg/Andr36oidPkg.dsc.inc

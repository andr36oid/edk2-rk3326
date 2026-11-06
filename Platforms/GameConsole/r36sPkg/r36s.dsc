##
#  andr36oid RK3326 handhelds: R36S and its clones, R36S Plus, R46H, R50S, ...
#  One image for all of them: RAM size, framebuffer and board come from the
#  hardware at run time.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME                  = r36s
  PLATFORM_GUID                  = 28F1A3BF-193A-47E3-A7B9-5A435EAAB2EE
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/r36sPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = r36sPkg/r36s.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 1

!include RK3326Pkg/RK3326Pkg.dsc.inc

[PcdsFixedAtBuild]
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x00000000

  # Right after the FD (base/size in Resources/Configs/r36s.toml)
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x02300000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"andr36oid"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"RK3326 handheld"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"r36s"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"r36s"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"rk3326"

  # Used by the on-screen debug log in SEC; the GOP driver reads the VOP.
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|640
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|480
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

[LibraryClasses]
  MemoryMapLib|r36sPkg/Library/MemoryMapLib/MemoryMapLib.inf
  BootKeysLib|r36sPkg/Library/BootKeysLib/BootKeysLib.inf
  KeypadDeviceLib|r36sPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf

[Components]
  # The null instance is not mapped, built here so it keeps compiling
  Andr36oidPkg/Library/BootKeysLibNull/BootKeysLibNull.inf

  # d-pad and A/B as text input
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf

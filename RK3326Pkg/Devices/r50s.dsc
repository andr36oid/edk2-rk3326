[Defines]
  PLATFORM_NAME                  = R50S
  PLATFORM_GUID                  = 28f1a3bf-193a-47e3-a7b9-5a435eaab2ee
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = RK3326Pkg/RK3326Pkg.fdf

!include RK3326Pkg/RK3326Pkg.dsc

[PcdsFixedAtBuild.common]
  # System Memory (1 GB)
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x00000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x40000000
  # Framebuffer (480x854)
  gRK3326PkgTokenSpaceGuid.PcdMipiFrameBufferAddress|0x3de00000
  gRK3326PkgTokenSpaceGuid.PcdMipiFrameBufferWidth|480
  gRK3326PkgTokenSpaceGuid.PcdMipiFrameBufferHeight|854
  gRK3326PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleWidth|480
  gRK3326PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleHeight|854
  gRK3326PkgTokenSpaceGuid.PcdMipiFrameBufferPixelBpp|32
  gRK3326PkgTokenSpaceGuid.PcdDisplayRotation|0

  [PcdsDynamicDefault.common]
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoHorizontalResolution|480 # /8 = column
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoVerticalResolution|854 #/19 = row
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoHorizontalResolution|480
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoVerticalResolution|854
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|60
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|44
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|60
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|44
##
#  andr36oid boot logic, shared by every device that boots andr36oid (r36s,
#  the QEMU test device). Include it at the end of the device .dsc so these
#  mappings win over SiliciumPkg's defaults. The device maps BootPlatformLib,
#  BootKeysLib and RkPmicLib; its .fdf includes Andr36oidPkg/Andr36oid.fdf.inc.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[PcdsFixedAtBuild]
  # The boot app comes first in BootOrder; this is BDS's wait before it.
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|1

[LibraryClasses]
  LinuxBootLib|Andr36oidPkg/Library/LinuxBootLib/LinuxBootLib.inf
  BootPolicyLib|Andr36oidPkg/Library/BootPolicyLib/BootPolicyLib.inf
  DeviceBootManagerLib|Andr36oidPkg/Library/DeviceBootManagerLib/DeviceBootManagerLib.inf

[Components]
  Andr36oidPkg/Application/Andr36oidBoot/Andr36oidBoot.inf

#
# RELEASE builds keep errors, warnings and DEBUG_INIT lines (the RK log lines)
# so BOOT/uefi-log.txt still says why a start failed. The fixed level lets the
# compiler drop every other DEBUG () string; ASSERTs stay off.
#
!if $(TARGET) == RELEASE
[LibraryClasses]
  DebugLib|MdePkg/Library/BaseDebugLibSerialPort/BaseDebugLibSerialPort.inf

[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x02
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000003
  gEfiMdePkgTokenSpaceGuid.PcdFixedDebugPrintErrorLevel|0x80000003

[BuildOptions]
  RELEASE_CLANGPDB_*_CC_FLAGS = -U MDEPKG_NDEBUG
!endif

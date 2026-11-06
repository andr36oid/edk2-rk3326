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

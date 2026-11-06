##
#  andr36oid boot logic, shared by every device that boots andr36oid (r36s,
#  the QEMU test device). Include it at the end of the device .dsc so these
#  mappings win over SiliciumPkg's defaults.
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[LibraryClasses]
  LinuxBootLib|Andr36oidPkg/Library/LinuxBootLib/LinuxBootLib.inf

[Components]
  Andr36oidPkg/Library/LinuxBootLib/LinuxBootLib.inf

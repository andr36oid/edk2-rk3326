//
// QEMU virt SEC entry hook: qemu -kernel enters with x0 = its FDT, like
// U-Boot's booti on the RK3326. Keep it for the log.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

ASM_FUNC (PlatformAssemblyInitialize)
  adrp  x1, ASM_PFX (gQemuVirtFdtArg)
  add   x1, x1, :lo12:ASM_PFX (gQemuVirtFdtArg)
  str   x0, [x1]
  // Use SP_ELx
  msr   SPSel, #1
  ret

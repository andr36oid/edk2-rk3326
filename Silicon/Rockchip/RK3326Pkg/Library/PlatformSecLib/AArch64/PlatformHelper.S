//
// RK3326 SEC entry hook. U-Boot's booti enters with x0 = FDT; keep it.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

ASM_FUNC (PlatformAssemblyInitialize)
  adrp  x1, ASM_PFX (gRkBootloaderFdtArg)
  add   x1, x1, :lo12:ASM_PFX (gRkBootloaderFdtArg)
  str   x0, [x1]
  // Use SP_ELx
  msr   SPSel, #1
  ret

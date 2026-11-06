//
// Enter Linux the way booti does (arm64 booting.rst): interrupts masked,
// MMU and caches off at the current EL, everything cleaned to the point of
// coherency, x0 = FDT, x1-x3 = 0.
//
// No stack and no memory writes once the caches are cleaned: anything
// written after the clean would sit dirty in a cache that is about to be
// switched off. UEFI maps RAM 1:1, so turning the MMU off does not move the
// code we run.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

#define SCTLR_M   (1 << 0)
#define SCTLR_C   (1 << 2)
#define SCTLR_I   (1 << 12)

// VOID LinuxJumpToKernel (UINT64 Entry, UINT64 Fdt)
ASM_FUNC (LinuxJumpToKernel)
  mov   x19, x0
  mov   x20, x1
  msr   daifset, #0xf
  isb

  bl    CleanInvalidateAllDataCaches

  mov   x2, #(SCTLR_M | SCTLR_C | SCTLR_I)
  mrs   x0, CurrentEL
  cmp   x0, #(2 << 2)
  b.eq  1f
  b.hi  2f
  mrs   x1, sctlr_el1
  bic   x1, x1, x2
  msr   sctlr_el1, x1
  b     3f
1:
  mrs   x1, sctlr_el2
  bic   x1, x1, x2
  msr   sctlr_el2, x1
  b     3f
2:
  mrs   x1, sctlr_el3
  bic   x1, x1, x2
  msr   sctlr_el3, x1
3:
  isb

  // Lines the CPU pulled in between the clean and the switch are clean;
  // drop them so the kernel never sees a stale copy.
  bl    CleanInvalidateAllDataCaches
  ic    iallu
  dsb   sy
  isb

  mov   x0, x20
  mov   x1, xzr
  mov   x2, xzr
  mov   x3, xzr
  br    x19

//
// Clean and invalidate every data/unified cache level up to the level of
// coherency by set/way (DC CISW). Leaf: uses x0-x11 and x30 only.
//
CleanInvalidateAllDataCaches:
  dsb   sy
  mrs   x0, clidr_el1
  ubfx  x3, x0, #24, #3           // LoC
  lsl   x3, x3, #1                // LoC * 2, the CSSELR level encoding
  cbz   x3, 5f
  mov   x10, #0                   // level * 2
1:
  add   x2, x10, x10, lsr #1      // level * 3: Ctype field position
  lsr   x1, x0, x2
  and   x1, x1, #7
  cmp   x1, #2                    // 0 none, 1 instruction only
  b.lt  4f
  msr   csselr_el1, x10
  isb
  mrs   x1, ccsidr_el1
  and   x2, x1, #7
  add   x2, x2, #4                // log2 (line size in bytes)
  ubfx  x4, x1, #3, #10           // ways - 1
  clz   w5, w4                    // way field shift
  ubfx  x7, x1, #13, #15          // sets - 1
2:
  mov   x9, x4
3:
  lsl   x6, x9, x5
  orr   x11, x10, x6
  lsl   x6, x7, x2
  orr   x11, x11, x6
  dc    cisw, x11
  subs  x9, x9, #1
  b.ge  3b
  subs  x7, x7, #1
  b.ge  2b
4:
  add   x10, x10, #2
  cmp   x3, x10
  b.gt  1b
5:
  mov   x10, #0
  msr   csselr_el1, x10
  dsb   sy
  isb
  ret

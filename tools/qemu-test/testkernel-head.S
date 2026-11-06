//
// Bare-metal stand-in for Android's kernel: an arm64 Image header (booting.rst)
// and a few lines that keep what the boot loader handed over, then C.
//
#define TEXT_OFFSET  0x80000
#define IMAGE_SIZE   0x400000      // more than the file: the loader must keep it free
#define STACK_TOP    0x3f0000      // inside IMAGE_SIZE, past the end of the file

  .section .text.head, "ax"
  .globl _head
_head:
  b     start                     // code0
  .long 0                         // code1
  .quad TEXT_OFFSET               // text_offset
  .quad IMAGE_SIZE                // image_size
  .quad 0x2                       // flags: little endian, 4K pages, 2 MB aligned base
  .quad 0, 0, 0
  .ascii "ARM\x64"
  .long 0

start:
  mov   x19, x0
  mov   x20, x1
  mov   x21, x2
  mov   x22, x3
  mrs   x23, daif
  mrs   x24, CurrentEL
  lsr   x24, x24, #2
  cmp   x24, #2
  b.eq  1f
  mrs   x25, sctlr_el1
  b     2f
1:
  mrs   x25, sctlr_el2
2:
  adr   x26, _head
  ldr   x9, =STACK_TOP
  add   x9, x9, x26
  mov   sp, x9

  mov   x0, x19                   // FDT
  mov   x1, x20
  mov   x2, x21
  mov   x3, x22
  mov   x4, x23                   // DAIF
  mov   x5, x24                   // EL
  mov   x6, x25                   // SCTLR at that EL
  mov   x7, x26                   // where we run
  bl    test_main
3:
  wfi
  b     3b

  .globl psci_call
psci_call:                        // x0 = function, x1 = 1 for SMC (EL2), 0 for HVC
  cbz   x1, 4f
  mov   x1, xzr
  smc   #0
  ret
4:
  hvc   #0
  ret
  .ltorg

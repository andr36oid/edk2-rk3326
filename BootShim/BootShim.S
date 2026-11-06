_Head:
  adr   x1, _Payload    /* Set _Entry address */
  b     _Start          /* Jump to the real code */

#if REQUIRES_KERNEL_HEADER == 1
#ifndef KERNEL_FLAGS
#define KERNEL_FLAGS 0
#endif

/*
 * IN_PLACE: the header asks the loader (U-Boot booti) to put the image so
 * that _Payload lands exactly on FD_BASE; the shim then jumps without
 * copying. Without it a loader that puts the image at FD_BASE makes the
 * copy below overwrite the running copy loop.
 */
#if IN_PLACE == 1
_StackBase:
  .quad FD_BASE - (_Payload - _Head)  /* Text Offset */

_StackSize:
  .quad FD_SIZE + (_Payload - _Head)  /* Image Size */
#else
_StackBase:
  .quad FD_BASE         /* Text Offset */

_StackSize:
  .quad FD_SIZE         /* Image Size */
#endif
  .quad KERNEL_FLAGS    /* Flags */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .ascii "ARM\x64"      /* ARM64 Magic */
  .long 0               /* Reserved */
#endif

_Start:
  mov   x4, x1
  ldr   x5, =FD_BASE
  cmp   x4, x5
  beq   _Entry
  ldr   x6,  =FD_SIZE

_CopyLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _CopyLoop
  dsb   sy              /* copy done before fetching from it */
  ic    iallu           /* the loader may have left the I-cache on */
  dsb   sy
  isb
  ldr   x5, =FD_BASE

_Entry:
  br    x5

_Dead:
  b     _Dead

/* The ldr literals go here, not after _Payload where the FD is appended. */
  .ltorg

.text
#if IN_PLACE == 1
.balign 256             /* fixed FD offset: the load address stays FD_BASE - 0x100 */
#else
.align 4
#endif

_Payload:

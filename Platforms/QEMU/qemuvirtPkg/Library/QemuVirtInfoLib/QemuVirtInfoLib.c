/** @file
  RAM size and FDT of the QEMU virt test device, read from the FDT qemu
  left in its window.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Guid/QemuVirtPlatformInfoHob.h>

VOID
EFIAPI
QemuVirtGetPlatformInfo (
  OUT QEMU_VIRT_PLATFORM_INFO  *Info
  )
{
  CONST VOID   *Fdt = (CONST VOID *)(UINTN)QEMU_VIRT_FDT_BASE;
  CONST UINT8  *Reg;
  INT32        Node;
  INT32        Len;
  UINT64       Base;
  UINT64       Size;
  INTN         I;

  ZeroMem (Info, sizeof (*Info));
  Info->RamBase = QEMU_VIRT_RAM_BASE;
  Info->RamSize = SIZE_1GB;

  if ((FdtCheckHeader (Fdt) != 0) || (FdtTotalSize (Fdt) > QEMU_VIRT_FDT_SIZE)) {
    return;
  }

  Info->Fdt     = QEMU_VIRT_FDT_BASE;
  Info->FdtSize = FdtTotalSize (Fdt);

  // qemu writes one /memory@40000000 with 2/2 cells.
  Node = FdtNodeOffsetByPropValue (Fdt, -1, "device_type", "memory", sizeof ("memory"));
  Reg  = (Node >= 0) ? FdtGetProp (Fdt, Node, "reg", &Len) : NULL;
  if ((Reg != NULL) && (Len >= 16)) {
    Base = 0;
    Size = 0;
    for (I = 0; I < 8; I++) {
      Base = (Base << 8) | Reg[I];
      Size = (Size << 8) | Reg[8 + I];
    }

    if ((Base == QEMU_VIRT_RAM_BASE) && (Size >= SIZE_512MB)) {
      Info->RamSize = Size;
    }
  }
}

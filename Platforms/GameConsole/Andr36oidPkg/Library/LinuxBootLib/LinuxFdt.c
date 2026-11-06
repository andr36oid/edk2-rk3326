/** @file
  FDT fixups for the Linux handoff: what U-Boot's booti does to the DTB
  (common/image-fdt.c, fdt_support.c, the Rockchip logo handover). No boot
  services: the host tests build this file with gcc.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/LinuxBootLib.h>

#define DRM_LOGO_COMPATIBLE  "rockchip,drm-logo"
#define ROUTE_PATH           "/display-subsystem/route"

STATIC
UINT32
Cells (
  IN CONST VOID  *Fdt,
  IN INT32       Node,
  IN BOOLEAN     Size
  )
{
  INT32  N;

  N = Size ? FdtSizeCells (Fdt, Node) : FdtAddressCells (Fdt, Node);
  if ((N < 1) || (N > 2)) {
    // Our trees use 2/2; 1 is the only other value that makes sense here.
    N = Size ? 1 : 2;
  }

  return (UINT32)N;
}

STATIC
UINT64
ReadCells (
  IN CONST UINT8  *P,
  IN UINT32       N
  )
{
  UINT64  V = 0;
  UINT32  I;

  for (I = 0; I < N * 4; I++) {
    V = (V << 8) | P[I];
  }

  return V;
}

STATIC
UINT8 *
WriteCells (
  OUT UINT8   *P,
  IN  UINT32  N,
  IN  UINT64  V
  )
{
  INT32  I;

  for (I = (INT32)(N * 4) - 1; I >= 0; I--) {
    P[I] = (UINT8)V;
    V  >>= 8;
  }

  return P + N * 4;
}

STATIC
BOOLEAN
IsMemoryNode (
  IN CONST VOID  *Fdt,
  IN INT32       Node
  )
{
  CONST CHAR8  *Type;
  INT32        Len;

  Type = FdtGetProp (Fdt, Node, "device_type", &Len);
  return (Type != NULL) && (Len >= 7) && (AsciiStrnCmp (Type, "memory", 7) == 0);
}

STATIC
BOOLEAN
Overlaps (
  IN UINT64  ABase,
  IN UINT64  ASize,
  IN UINT64  BBase,
  IN UINT64  BSize
  )
{
  return (ASize != 0) && (BSize != 0) && (ABase < BBase + BSize) && (BBase < ABase + ASize);
}

UINTN
EFIAPI
LinuxFdtGetMemory (
  IN  CONST VOID          *Fdt,
  OUT LINUX_MEMORY_RANGE  *Ranges,
  IN  UINTN               MaxRanges
  )
{
  INT32        Node;
  INT32        Depth;
  INT32        Len;
  UINT32       Ac;
  UINT32       Sc;
  CONST UINT8  *Reg;
  UINTN        Count = 0;

  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0)) {
    return 0;
  }

  Ac    = Cells (Fdt, 0, FALSE);
  Sc    = Cells (Fdt, 0, TRUE);
  Depth = 0;
  for (Node = FdtNextNode (Fdt, 0, &Depth); Node >= 0 && Depth > 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
    if ((Depth != 1) || !IsMemoryNode (Fdt, Node)) {
      continue;
    }

    Reg = FdtGetProp (Fdt, Node, "reg", &Len);
    while ((Reg != NULL) && (Len >= (INT32)((Ac + Sc) * 4)) && (Count < MaxRanges)) {
      Ranges[Count].Base = ReadCells (Reg, Ac);
      Ranges[Count].Size = ReadCells (Reg + Ac * 4, Sc);
      if (Ranges[Count].Size != 0) {
        Count++;
      }

      Reg += (Ac + Sc) * 4;
      Len -= (INT32)((Ac + Sc) * 4);
    }
  }

  return Count;
}

CONST CHAR8 *
EFIAPI
LinuxFdtGetBootargs (
  IN CONST VOID  *Fdt
  )
{
  INT32        Node;
  INT32        Len;
  CONST CHAR8  *Args;

  if ((Fdt == NULL) || (FdtCheckHeader (Fdt) != 0)) {
    return NULL;
  }

  Node = FdtPathOffset (Fdt, "/chosen");
  if (Node < 0) {
    return NULL;
  }

  Args = FdtGetProp (Fdt, Node, "bootargs", &Len);
  if ((Args == NULL) || (Len < 1) || (Args[Len - 1] != '\0')) {
    return NULL;
  }

  return Args;
}

BOOLEAN
EFIAPI
LinuxFdtRangeConflicts (
  IN  CONST VOID                *Fdt,
  IN  CONST LINUX_MEMORY_RANGE  *Memory OPTIONAL,
  IN  UINTN                     MemoryCount,
  IN  UINT64                    Base,
  IN  UINT64                    Size,
  OUT CONST CHAR8               **Name OPTIONAL
  )
{
  INT32        Parent;
  INT32        Node;
  INT32        Len;
  INTN         Index;
  UINT32       Ac;
  UINT32       Sc;
  CONST UINT8  *Reg;
  UINT64       RBase;
  UINT64       RSize;
  UINTN        I;
  BOOLEAN      Inside;

  if (Name != NULL) {
    *Name = NULL;
  }

  if ((Memory != NULL) && (MemoryCount > 0)) {
    Inside = FALSE;
    for (I = 0; I < MemoryCount; I++) {
      if ((Base >= Memory[I].Base) && (Base + Size <= Memory[I].Base + Memory[I].Size)) {
        Inside = TRUE;
      }
    }

    if (!Inside) {
      if (Name != NULL) {
        *Name = "outside /memory";
      }

      return TRUE;
    }
  }

  for (Index = 0; Index < FdtGetNumberOfReserveMapEntries (Fdt); Index++) {
    if ((FdtGetReserveMapEntry (Fdt, Index, &RBase, &RSize) == 0) && Overlaps (Base, Size, RBase, RSize)) {
      if (Name != NULL) {
        *Name = "/memreserve/";
      }

      return TRUE;
    }
  }

  Parent = FdtPathOffset (Fdt, "/reserved-memory");
  if (Parent < 0) {
    return FALSE;
  }

  Ac = Cells (Fdt, Parent, FALSE);
  Sc = Cells (Fdt, Parent, TRUE);
  FdtForEachSubnode (Node, Fdt, Parent) {
    Reg = FdtGetProp (Fdt, Node, "reg", &Len);
    while ((Reg != NULL) && (Len >= (INT32)((Ac + Sc) * 4))) {
      RBase = ReadCells (Reg, Ac);
      RSize = ReadCells (Reg + Ac * 4, Sc);
      if (Overlaps (Base, Size, RBase, RSize)) {
        if (Name != NULL) {
          *Name = FdtGetName (Fdt, Node, NULL);
        }

        return TRUE;
      }

      Reg += (Ac + Sc) * 4;
      Len -= (INT32)((Ac + Sc) * 4);
    }
  }

  return FALSE;
}

STATIC
INT32
FindOrAdd (
  IN OUT VOID         *Fdt,
  IN     INT32        Parent,
  IN     CONST CHAR8  *Name
  )
{
  INT32  Node;

  Node = FdtSubnodeOffset (Fdt, Parent, Name);
  if (Node == -FDT_ERR_NOTFOUND) {
    Node = FdtAddSubnode (Fdt, Parent, Name);
  }

  return Node;
}

STATIC
INT32
SetChosen (
  IN OUT VOID                   *Fdt,
  IN     CONST LINUX_FDT_FIXUP  *Fixup
  )
{
  INT32   Chosen;
  INT32   Err;
  UINT8   Buf[8];
  UINT32  Ac;

  Chosen = FindOrAdd (Fdt, 0, "chosen");
  if (Chosen < 0) {
    return Chosen;
  }

  Err = FdtSetPropString (Fdt, Chosen, "bootargs", (Fixup->Bootargs != NULL) ? Fixup->Bootargs : "");
  if (Err != 0) {
    return Err;
  }

  if (Fixup->InitrdSize == 0) {
    FdtDelProp (Fdt, Chosen, "linux,initrd-start");
    FdtDelProp (Fdt, Chosen, "linux,initrd-end");
    return 0;
  }

  // U-Boot's fdt_initrd: 64-bit values when the root has #address-cells = 2.
  Ac = (FdtAddressCells (Fdt, 0) == 2) ? 2 : 1;
  WriteCells (Buf, Ac, Fixup->InitrdStart);
  Err = FdtSetProp (Fdt, Chosen, "linux,initrd-start", Buf, Ac * 4);
  if (Err != 0) {
    return Err;
  }

  WriteCells (Buf, Ac, Fixup->InitrdStart + Fixup->InitrdSize);
  return FdtSetProp (Fdt, Chosen, "linux,initrd-end", Buf, Ac * 4);
}

STATIC
INT32
SetMemory (
  IN OUT VOID                   *Fdt,
  IN     CONST LINUX_FDT_FIXUP  *Fixup
  )
{
  INT32   Node;
  INT32   Depth;
  INT32   Keep;
  UINT32  Ac;
  UINT32  Sc;
  UINT8   Reg[LINUX_MAX_MEMORY_RANGES * 16];
  UINT8   *P;
  UINTN   I;

  if ((Fixup->Memory == NULL) || (Fixup->MemoryCount == 0)) {
    return 0;
  }

  if (Fixup->MemoryCount > LINUX_MAX_MEMORY_RANGES) {
    return -FDT_ERR_BADVALUE;
  }

  //
  // One memory node, like U-Boot's fdt_fixup_memory_banks: the first one found
  // is reused, the others go (a stale bank in a second node would hand RAM to
  // Linux that is not there or not ours).
  //
  Keep = -1;
  do {
    Depth = 0;
    for (Node = FdtNextNode (Fdt, 0, &Depth); Node >= 0 && Depth > 0; Node = FdtNextNode (Fdt, Node, &Depth)) {
      if ((Depth == 1) && IsMemoryNode (Fdt, Node) && (Node != Keep)) {
        break;
      }
    }

    if ((Node < 0) || (Depth <= 0)) {
      break;
    }

    if (Keep < 0) {
      Keep = Node;
    } else {
      // Deleting moves later nodes, Keep sits before Node so it stays put.
      FdtDelNode (Fdt, Node);
    }
  } while (TRUE);

  if (Keep < 0) {
    Keep = FdtAddSubnode (Fdt, 0, "memory");
    if (Keep < 0) {
      return Keep;
    }
  }

  Ac = Cells (Fdt, 0, FALSE);
  Sc = Cells (Fdt, 0, TRUE);
  P  = Reg;
  for (I = 0; I < Fixup->MemoryCount; I++) {
    P = WriteCells (P, Ac, Fixup->Memory[I].Base);
    P = WriteCells (P, Sc, Fixup->Memory[I].Size);
  }

  if (FdtSetPropString (Fdt, Keep, "device_type", "memory") != 0) {
    return -FDT_ERR_NOSPACE;
  }

  return FdtSetProp (Fdt, Keep, "reg", Reg, (UINT32)(P - Reg));
}

STATIC
BOOLEAN
IsHandoverProperty (
  IN CONST CHAR8  *Name
  )
{
  return (AsciiStrnCmp (Name, "logo,", 5) == 0) ||
         (AsciiStrnCmp (Name, "video,", 6) == 0) ||
         (AsciiStrnCmp (Name, "overscan,", 9) == 0);
}

/**
  Copy logo,*, video,* and overscan,* from the route nodes of Src to the
  route nodes of the same name in Dst (rockchip_display_fixup puts them on
  the route that drove the screen, route_dsi on our boards).
**/
STATIC
INT32
CopyRouteProperties (
  IN OUT VOID        *Dst,
  IN     CONST VOID  *Src
  )
{
  INT32               SrcRoute;
  INT32               DstRoute;
  INT32               SrcNode;
  INT32               DstNode;
  INT32               Prop;
  INT32               Len;
  INT32               Err;
  CONST FDT_PROPERTY  *P;
  CONST CHAR8         *Name;
  CONST CHAR8         *PropName;
  INT32               Copied = 0;

  SrcRoute = FdtPathOffset (Src, ROUTE_PATH);
  DstRoute = FdtPathOffset (Dst, ROUTE_PATH);
  if ((SrcRoute < 0) || (DstRoute < 0)) {
    return 0;
  }

  FdtForEachSubnode (SrcNode, Src, SrcRoute) {
    if (FdtGetProp (Src, SrcNode, "logo,offset", NULL) == NULL) {
      continue;
    }

    Name    = FdtGetName (Src, SrcNode, NULL);
    DstNode = FdtSubnodeOffset (Dst, DstRoute, Name);
    if (DstNode < 0) {
      continue;
    }

    FdtForEachPropertyOffset (Prop, Src, SrcNode) {
      P = FdtGetPropertyByOffset (Src, Prop, &Len);
      if (P == NULL) {
        continue;
      }

      PropName = FdtGetString (Src, Fdt32ToCpu (P->NameOffset), NULL);
      if ((PropName == NULL) || !IsHandoverProperty (PropName)) {
        continue;
      }

      // Setting a property moves nodes in Dst; look the node up again.
      Err = FdtSetProp (Dst, DstNode, PropName, P->Data, (UINT32)Len);
      if (Err != 0) {
        return Err;
      }

      DstRoute = FdtPathOffset (Dst, ROUTE_PATH);
      DstNode  = FdtSubnodeOffset (Dst, DstRoute, Name);
      Copied++;
    }
  }

  return Copied;
}

STATIC
BOOLEAN
GetLogoRegion (
  IN  CONST VOID  *Fdt,
  OUT UINT64      *Base,
  OUT UINT64      *Size
  )
{
  INT32        Node;
  INT32        Parent;
  INT32        Len;
  UINT32       Ac;
  UINT32       Sc;
  CONST UINT8  *Reg;

  Node = FdtNodeOffsetByCompatible (Fdt, -1, DRM_LOGO_COMPATIBLE);
  if (Node < 0) {
    return FALSE;
  }

  Parent = FdtParentOffset (Fdt, Node);
  Ac     = Cells (Fdt, Parent, FALSE);
  Sc     = Cells (Fdt, Parent, TRUE);
  Reg    = FdtGetProp (Fdt, Node, "reg", &Len);
  if ((Reg == NULL) || (Len < (INT32)((Ac + Sc) * 4))) {
    return FALSE;
  }

  *Base = ReadCells (Reg, Ac);
  *Size = ReadCells (Reg + Ac * 4, Sc);
  return *Size != 0;
}

STATIC
INT32
SetLogo (
  IN OUT VOID                   *Fdt,
  IN     CONST LINUX_FDT_FIXUP  *Fixup
  )
{
  INT32   Node;
  INT32   Parent;
  UINT32  Ac;
  UINT32  Sc;
  UINT8   Reg[16];
  UINT8   *P;
  UINT64  Base = 0;
  UINT64  Size = 0;
  INT32   Err;

  // Only where the DTB asks for a handover (the kernel looks this node up).
  Node = FdtNodeOffsetByCompatible (Fdt, -1, DRM_LOGO_COMPATIBLE);
  if (Node < 0) {
    return 0;
  }

  if ((Fixup->HandoverFdt != NULL) && (FdtCheckHeader (Fixup->HandoverFdt) == 0) &&
      GetLogoRegion (Fixup->HandoverFdt, &Base, &Size))
  {
    Err = CopyRouteProperties (Fdt, Fixup->HandoverFdt);
    if (Err < 0) {
      return Err;
    }
  } else if (Fixup->FbSize != 0) {
    Base = Fixup->FbBase;
    Size = Fixup->FbSize;
  } else {
    return 0;
  }

  Node   = FdtNodeOffsetByCompatible (Fdt, -1, DRM_LOGO_COMPATIBLE);
  Parent = FdtParentOffset (Fdt, Node);
  Ac     = Cells (Fdt, Parent, FALSE);
  Sc     = Cells (Fdt, Parent, TRUE);
  P      = WriteCells (Reg, Ac, Base);
  P      = WriteCells (P, Sc, Size);
  return FdtSetProp (Fdt, Node, "reg", Reg, (UINT32)(P - Reg));
}

RETURN_STATUS
EFIAPI
LinuxFdtFixup (
  IN OUT VOID                   *Fdt,
  IN     UINTN                  BufferSize,
  IN     CONST LINUX_FDT_FIXUP  *Fixup
  )
{
  if ((Fdt == NULL) || (Fixup == NULL) || (FdtCheckHeader (Fdt) != 0) ||
      (FdtTotalSize (Fdt) > BufferSize))
  {
    return RETURN_INVALID_PARAMETER;
  }

  if ((SetChosen (Fdt, Fixup) != 0) || (SetMemory (Fdt, Fixup) != 0) || (SetLogo (Fdt, Fixup) != 0)) {
    return RETURN_BUFFER_TOO_SMALL;
  }

  FdtPack (Fdt);
  return RETURN_SUCCESS;
}

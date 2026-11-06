// FdtLib (edk2) over the vendored libfdt, for the functions the PMIC driver
// uses. The real BaseFdtLib wraps libfdt the same way one-to-one.
#include <Uefi.h>
#include <Library/FdtLib.h>
#include <libfdt.h>

UINT32 EFIAPI Fdt32ToCpu (IN UINT32 Value) { return fdt32_to_cpu (Value); }

CONST CHAR8 * EFIAPI FdtGetName (IN CONST VOID *Fdt, IN INT32 NodeOffset, IN INT32 *Length)
{
  return fdt_get_name (Fdt, NodeOffset, Length);
}

CONST VOID * EFIAPI FdtGetProp (IN CONST VOID *Fdt, IN INT32 NodeOffset, IN CONST CHAR8 *Name, IN INT32 *Length)
{
  return fdt_getprop (Fdt, NodeOffset, Name, Length);
}

INT32 EFIAPI FdtNodeOffsetByCompatible (IN CONST VOID *Fdt, IN INT32 StartOffset, IN CONST CHAR8 *Compatible)
{
  return fdt_node_offset_by_compatible (Fdt, StartOffset, Compatible);
}

INT32 EFIAPI FdtSubnodeOffset (IN CONST VOID *Fdt, IN INT32 ParentOffset, IN CONST CHAR8 *Name)
{
  return fdt_subnode_offset (Fdt, ParentOffset, Name);
}

INT32 EFIAPI FdtFirstSubnode (IN CONST VOID *Fdt, IN INT32 Offset) { return fdt_first_subnode (Fdt, Offset); }
INT32 EFIAPI FdtNextSubnode (IN CONST VOID *Fdt, IN INT32 Offset) { return fdt_next_subnode (Fdt, Offset); }

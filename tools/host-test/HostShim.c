/** @file
  The few BaseLib / BaseMemoryLib / DebugLib functions the pure andr36oid
  libraries and MdePkg's FdtLib wrapper call, on top of the host's libc, so
  they build with gcc for host tests.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

VOID *EFIAPI CopyMem (OUT VOID *D, IN CONST VOID *S, IN UINTN L) { return memmove (D, S, L); }
VOID *EFIAPI SetMem (OUT VOID *B, IN UINTN L, IN UINT8 V) { return memset (B, V, L); }
VOID *EFIAPI ZeroMem (OUT VOID *B, IN UINTN L) { return memset (B, 0, L); }
INTN EFIAPI CompareMem (IN CONST VOID *A, IN CONST VOID *B, IN UINTN L) { return memcmp (A, B, L); }
VOID *EFIAPI ScanMem8 (IN CONST VOID *B, IN UINTN L, IN UINT8 V) { return memchr (B, V, L); }
UINTN EFIAPI AsciiStrLen (IN CONST CHAR8 *S) { return strlen (S); }
UINTN EFIAPI AsciiStrSize (IN CONST CHAR8 *S) { return strlen (S) + 1; }
UINTN EFIAPI AsciiStrnLenS (IN CONST CHAR8 *S, IN UINTN M) { return (S == NULL) ? 0 : strnlen (S, M); }
INTN EFIAPI AsciiStrCmp (IN CONST CHAR8 *A, IN CONST CHAR8 *B) { return strcmp (A, B); }
INTN EFIAPI AsciiStrnCmp (IN CONST CHAR8 *A, IN CONST CHAR8 *B, IN UINTN L) { return strncmp (A, B, L); }
UINT16 EFIAPI SwapBytes16 (IN UINT16 V) { return __builtin_bswap16 (V); }
UINT32 EFIAPI SwapBytes32 (IN UINT32 V) { return __builtin_bswap32 (V); }
UINT64 EFIAPI SwapBytes64 (IN UINT64 V) { return __builtin_bswap64 (V); }

VOID
EFIAPI
DebugPrint (
  IN UINTN        Level,
  IN CONST CHAR8  *Format,
  ...
  )
{
}

VOID
EFIAPI
DebugAssert (
  IN CONST CHAR8  *File,
  IN UINTN        Line,
  IN CONST CHAR8  *Description
  )
{
  fprintf (stderr, "ASSERT %s:%u %s\n", File, (unsigned)Line, Description);
  abort ();
}

BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (IN CONST UINTN L) { return FALSE; }
BOOLEAN EFIAPI DebugCodeEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugClearMemoryEnabled (VOID) { return FALSE; }

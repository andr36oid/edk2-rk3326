// Shared mocks: debug output, delays, and a small register-access hook.
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/BaseMemoryLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mock_verbose;

VOID EFIAPI DebugPrint (UINTN ErrorLevel, CONST CHAR8 *Format, ...)
{
  // The arguments are not decoded (edk2 %r/%a/%lX need its own formatter);
  // with mock_verbose the format strings alone show which path ran.
  if (mock_verbose) {
    fputs (Format, stdout);
  }
}

VOID EFIAPI DebugAssert (CONST CHAR8 *FileName, UINTN LineNumber, CONST CHAR8 *Description)
{
  fprintf (stderr, "ASSERT %s:%lu %s\n", FileName, (unsigned long)LineNumber, Description);
  abort ();
}

BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN ErrorLevel) { return TRUE; }
BOOLEAN EFIAPI DebugCodeEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugClearMemoryEnabled (VOID) { return TRUE; }
UINT8 EFIAPI DebugPropertyGet (VOID) { return 0; }
UINT8 EFIAPI DebugPropertySet (UINT8 a, UINT8 b) { return 0; }

UINTN mock_delay_us;
UINTN EFIAPI MicroSecondDelay (IN UINTN MicroSeconds) { mock_delay_us += MicroSeconds; return MicroSeconds; }
UINTN EFIAPI NanoSecondDelay (IN UINTN NanoSeconds) { return NanoSeconds; }
UINT64 EFIAPI GetPerformanceCounter (VOID) { return 0; }
UINT64 EFIAPI GetPerformanceCounterProperties (OUT UINT64 *S, OUT UINT64 *E) { return 0; }
UINT64 EFIAPI GetTimeInNanoSecond (IN UINT64 Ticks) { return Ticks; }

VOID * EFIAPI ZeroMem (OUT VOID *Buffer, IN UINTN Length) { memset (Buffer, 0, Length); return Buffer; }

// The world RkDisplayDxe sees, on the host: a register file that behaves
// enough like the PX30 (write-masked CRU/GRF/PMU registers, PLLs and the
// D-PHY that lock, DSI FIFOs that drain), libfdt behind FdtLib, a PMIC
// that records what it is asked, and DebugPrint with edk2's formats.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef FDT_TAGSIZE
#include <libfdt.h>
#include "fake_env.h"

int     fake_verbose;
int     fake_errors;
int     fake_warnings;
UINT64  fake_time_us;

// ---- DebugPrint: edk2 formats (%a %r %p, l for 64 bit) --------------------

static const char *
StatusName (UINT64 s)
{
  static char buf[32];

  switch (s) {
    case 0: return "Success";
    case 0x8000000000000002ULL: return "Invalid Parameter";
    case 0x8000000000000003ULL: return "Unsupported";
    case 0x8000000000000007ULL: return "Device Error";
    case 0x8000000000000009ULL: return "Out of Resources";
    case 0x800000000000000EULL: return "Not Found";
    case 0x8000000000000012ULL: return "Time out";
    case 0x8000000000000014ULL: return "Already started";
  }

  snprintf (buf, sizeof (buf), "status 0x%llx", (unsigned long long)s);
  return buf;
}

VOID EFIAPI
DebugPrint (UINTN ErrorLevel, CONST CHAR8 *Format, ...)
{
  va_list  ap;
  char     out[1024];
  size_t   n = 0;
  const char *f;

  if (ErrorLevel & DEBUG_ERROR) {
    fake_errors++;
  } else if (ErrorLevel & DEBUG_WARN) {
    fake_warnings++;
  }

  va_start (ap, Format);
  for (f = Format; *f != '\0' && n < sizeof (out) - 64; f++) {
    char  spec[16];
    int   s = 0, longarg = 0;

    if (*f != '%') {
      out[n++] = *f;
      continue;
    }

    spec[s++] = '%';
    for (f++; *f == '0' || *f == '-' || (*f >= '1' && *f <= '9'); f++) {
      spec[s++] = *f;
    }

    if (*f == 'l' || *f == 'L') {
      longarg = 1;
      f++;
    }

    switch (*f) {
      case 'a':
        spec[s++] = 's'; spec[s] = 0;
        n += snprintf (out + n, sizeof (out) - n, spec, va_arg (ap, const char *));
        break;
      case 'c':
        out[n++] = (char)va_arg (ap, int);
        break;
      case 'r':
        n += snprintf (out + n, sizeof (out) - n, "%s", StatusName (va_arg (ap, UINT64)));
        break;
      case 'p':
        n += snprintf (out + n, sizeof (out) - n, "%016llX", (unsigned long long)(UINTN)va_arg (ap, void *));  // as edk2 prints it
        break;
      case 'd': case 'u': case 'x': case 'X':
        spec[s++] = 'l'; spec[s++] = 'l'; spec[s++] = *f; spec[s] = 0;
        if (longarg) {
          n += snprintf (out + n, sizeof (out) - n, spec, va_arg (ap, unsigned long long));
        } else if (*f == 'd') {
          n += snprintf (out + n, sizeof (out) - n, spec, (long long)va_arg (ap, int));
        } else {
          n += snprintf (out + n, sizeof (out) - n, spec, (unsigned long long)va_arg (ap, unsigned int));
        }
        break;
      case '%':
        out[n++] = '%';
        break;
      default:
        n += snprintf (out + n, sizeof (out) - n, "<bad %%%c>", *f);
        fake_errors++;
        break;
    }
  }

  va_end (ap);
  out[n] = 0;
  if (fake_verbose) {
    fputs (out, stdout);
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

UINTN EFIAPI MicroSecondDelay (IN UINTN MicroSeconds) { fake_time_us += MicroSeconds; return MicroSeconds; }
UINTN EFIAPI NanoSecondDelay (IN UINTN NanoSeconds) { return NanoSeconds; }
UINT64 EFIAPI GetPerformanceCounter (VOID) { return fake_time_us; }
UINT64 EFIAPI GetPerformanceCounterProperties (OUT UINT64 *S, OUT UINT64 *E) { return 1000000; }
UINT64 EFIAPI GetTimeInNanoSecond (IN UINT64 Ticks) { return Ticks * 1000; }

// ---- BaseMemoryLib / BaseLib pieces ---------------------------------------

VOID * EFIAPI ZeroMem (OUT VOID *B, IN UINTN L) { memset (B, 0, L); return B; }
VOID * EFIAPI SetMem (OUT VOID *B, IN UINTN L, IN UINT8 V) { memset (B, V, L); return B; }
VOID * EFIAPI CopyMem (OUT VOID *D, IN CONST VOID *S, IN UINTN L) { memmove (D, S, L); return D; }
INTN EFIAPI CompareMem (IN CONST VOID *A, IN CONST VOID *B, IN UINTN L) { return memcmp (A, B, L); }
UINTN EFIAPI AsciiStrLen (IN CONST CHAR8 *S) { return strlen (S); }
INTN EFIAPI AsciiStrCmp (IN CONST CHAR8 *A, IN CONST CHAR8 *B) { return strcmp (A, B); }
INTN EFIAPI AsciiStrnCmp (IN CONST CHAR8 *A, IN CONST CHAR8 *B, IN UINTN L) { return strncmp (A, B, L); }
UINTN EFIAPI AsciiStrHexToUintn (IN CONST CHAR8 *S) { return strtoull (S, NULL, 16); }

// ---- Registers --------------------------------------------------------------

#define SLOTS  8192

static struct { UINTN Addr; UINT32 Val; int Used; } mRegs[SLOTS];
FAKE_WRITE  fake_trace[FAKE_TRACE_MAX];
int         fake_trace_count;

static int
Slot (UINTN Addr)
{
  unsigned h = (unsigned)((Addr >> 2) * 2654435761u) % SLOTS;

  while (mRegs[h].Used && mRegs[h].Addr != Addr) {
    h = (h + 1) % SLOTS;
  }

  if (!mRegs[h].Used) {
    mRegs[h].Used = 1;
    mRegs[h].Addr = Addr;
    mRegs[h].Val  = 0;
  }

  return h;
}

UINT32 fake_get (UINTN Addr) { return mRegs[Slot (Addr)].Val; }
void   fake_set (UINTN Addr, UINT32 Val) { mRegs[Slot (Addr)].Val = Val; }

void
fake_reset (void)
{
  memset (mRegs, 0, sizeof (mRegs));
  fake_trace_count = 0;
  fake_time_us     = 0;
  fake_errors      = 0;
  fake_warnings    = 0;
}

// Registers with a write mask in the upper half (rk_clrsetreg style).
static int
WriteMasked (UINTN Addr)
{
  return (Addr >= 0xFF2B0000 && Addr < 0xFF2B0400) ||    // CRU
         (Addr >= 0xFF2BC000 && Addr < 0xFF2BC100) ||    // PMUCRU
         (Addr >= 0xFF140000 && Addr < 0xFF141000) ||    // GRF
         (Addr >= 0xFF010000 && Addr < 0xFF011000) ||    // PMUGRF
         (Addr == 0xFF000018) || (Addr == 0xFF000064);   // PMU PWRDN_CON, BUS_IDLE_REQ
}

UINT32 EFIAPI
MmioRead32 (IN UINTN Address)
{
  UINT32  v = fake_get (Address);

  // PLL con1: locked
  if (((Address >= 0xFF2B0000 && Address < 0xFF2B0080) && ((Address & 0x1F) == 4)) || (Address == 0xFF2BC004)) {
    v |= 1u << 10;
  }

  // DSI PHY_STATUS: locked, clock and data lane 0 in stop state
  if (Address == 0xFF4500B0) {
    v |= (1u << 0) | (1u << 2) | (1u << 4);
  }

  // DSI CMD_PKT_STATUS: command and payload FIFOs empty
  if (Address == 0xFF450074) {
    v |= (1u << 0) | (1u << 2);
  }

  // PMU: status follows the request
  if (Address == 0xFF000020) {
    v = (fake_get (0xFF000018) & (1u << 13));
  }

  if (Address == 0xFF00006C) {
    v = (fake_get (0xFF000064) & (1u << 7)) ? ((1u << 7) | (1u << 23)) : 0;
  }

  // GPIO ext_port reads the pads the data register drives
  if ((Address & 0xFFF) == 0x50 && (Address == 0xFF040050 || (Address >= 0xFF250050 && Address <= 0xFF280050))) {
    v = fake_get (Address - 0x50);
  }

  return v;
}

UINT32 EFIAPI
MmioWrite32 (IN UINTN Address, IN UINT32 Value)
{
  UINT32  Old = fake_get (Address);
  UINT32  New;

  if (WriteMasked (Address)) {
    New = (Old & ~(Value >> 16)) | (Value & (Value >> 16));
  } else {
    New = Value;
  }

  fake_set (Address, New);
  if (fake_trace_count < FAKE_TRACE_MAX) {
    fake_trace[fake_trace_count].Addr  = Address;
    fake_trace[fake_trace_count].Value = Value;
    fake_trace[fake_trace_count].After = New;
    fake_trace[fake_trace_count].Time  = fake_time_us;
    fake_trace_count++;
  }

  return Value;
}

// ---- FdtLib over libfdt ----------------------------------------------------

UINT32 EFIAPI Fdt32ToCpu (IN UINT32 Value) { return fdt32_to_cpu (Value); }
INT32 EFIAPI FdtCheckHeader (IN CONST VOID *Fdt) { return fdt_check_header (Fdt); }
CONST CHAR8 * EFIAPI FdtGetName (IN CONST VOID *Fdt, IN INT32 Node, IN INT32 *Len) { return fdt_get_name (Fdt, Node, Len); }
CONST VOID * EFIAPI FdtGetProp (IN CONST VOID *Fdt, IN INT32 Node, IN CONST CHAR8 *Name, IN INT32 *Len) { return fdt_getprop (Fdt, Node, Name, Len); }
INT32 EFIAPI FdtNodeOffsetByCompatible (IN CONST VOID *Fdt, IN INT32 Start, IN CONST CHAR8 *C) { return fdt_node_offset_by_compatible (Fdt, Start, C); }
INT32 EFIAPI FdtSubnodeOffset (IN CONST VOID *Fdt, IN INT32 Parent, IN CONST CHAR8 *Name) { return fdt_subnode_offset (Fdt, Parent, Name); }
INT32 EFIAPI FdtFirstSubnode (IN CONST VOID *Fdt, IN INT32 Offset) { return fdt_first_subnode (Fdt, Offset); }
INT32 EFIAPI FdtNextSubnode (IN CONST VOID *Fdt, IN INT32 Offset) { return fdt_next_subnode (Fdt, Offset); }
INT32 EFIAPI FdtParentOffset (IN CONST VOID *Fdt, IN INT32 Node) { return fdt_parent_offset (Fdt, Node); }
INT32 EFIAPI FdtNodeOffsetByPhandle (IN CONST VOID *Fdt, IN UINT32 Phandle) { return fdt_node_offset_by_phandle (Fdt, Phandle); }
INT32 EFIAPI FdtPathOffset (IN CONST VOID *Fdt, IN CONST CHAR8 *Path) { return fdt_path_offset (Fdt, Path); }
INT32 EFIAPI FdtAddressCells (IN CONST VOID *Fdt, IN INT32 Node) { return fdt_address_cells (Fdt, Node); }
INT32 EFIAPI FdtNextNode (IN CONST VOID *Fdt, IN INT32 Offset, IN INT32 *Depth) { return fdt_next_node (Fdt, Offset, Depth); }
INT32 EFIAPI FdtFirstPropertyOffset (IN CONST VOID *Fdt, IN INT32 Node) { return fdt_first_property_offset (Fdt, Node); }
INT32 EFIAPI FdtNextPropertyOffset (IN CONST VOID *Fdt, IN INT32 Offset) { return fdt_next_property_offset (Fdt, Offset); }
CONST FDT_PROPERTY * EFIAPI FdtGetPropertyByOffset (IN CONST VOID *Fdt, IN INT32 Offset, IN INT32 *Len) { return (CONST FDT_PROPERTY *)fdt_get_property_by_offset (Fdt, Offset, Len); }
CONST CHAR8 * EFIAPI FdtGetString (IN CONST VOID *Fdt, IN INT32 StrOffset, IN INT32 *Len) { return fdt_get_string (Fdt, StrOffset, Len); }
UINT32 EFIAPI FdtGetPhandle (IN CONST VOID *Fdt, IN INT32 Node) { return fdt_get_phandle (Fdt, Node); }

// ---- RkPmicLib ---------------------------------------------------------------

FAKE_PMIC_CALL  fake_pmic[64];
int             fake_pmic_count;

EFI_STATUS EFIAPI
RkPmicSetRegulator (IN CONST VOID *Fdt, IN INT32 Node, IN BOOLEAN Enable)
{
  if (fake_pmic_count < 64) {
    fake_pmic[fake_pmic_count].Name   = fdt_get_name (Fdt, Node, NULL);
    fake_pmic[fake_pmic_count].Enable = Enable;
    fake_pmic[fake_pmic_count].Trace  = fake_trace_count;
    fake_pmic_count++;
  }

  return EFI_SUCCESS;
}

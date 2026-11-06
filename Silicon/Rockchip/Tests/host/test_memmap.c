// r36s MemoryMapLib for every RAM size, framebuffer and OP-TEE case the boot
// chain can leave. Expectations come from the memory-map rules in PLAN.md and
// AGENT-BRIEF.md (BL31, ramoops, log, FDT window, FD, stack, 80 MB DXE heap,
// OP-TEE at trust.img's BL32 load address, U-Boot's framebuffer reserved,
// registers from 0xFF000000), from U-Boot (RAM ends below the registers,
// SDRAM_MAX_SIZE) and from what Silicium's SEC does with the map (sorts it,
// dead-loops on overlaps, finds "DXE Heap" and "Display Reserved" by name,
// ArmConfigureMmu refuses anything that is not whole 4 KB pages).
// RkPlatformInfoLib is replaced by the facts each case sets.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent

#include <PiPei.h>
#include <Library/RkPlatformInfoLib.h>
#include <Library/RkLogLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "test.h"
#include "known.h"

// ---- the boot chain's leftovers, as RkPlatformInfoLib would report them -------

static UINT64               mDram;
static BOOLEAN              mHaveFb;
static RK_FRAMEBUFFER_INFO  mFb;
static EFI_STATUS           mTagStatus;
static RK_BOOT_TAGS         mTags;
static int                  mInfoCalls;

UINT64 EFIAPI RkGetDramSize (VOID) { mInfoCalls++; return mDram; }

EFI_STATUS EFIAPI RkGetActiveFramebuffer (OUT RK_FRAMEBUFFER_INFO *Info)
{
  mInfoCalls++;
  if (!mHaveFb) {
    return EFI_NOT_FOUND;
  }

  *Info = mFb;
  return EFI_SUCCESS;
}

// MemoryMapLib turns the screen log off when it drops a live framebuffer.
static BOOLEAN  mLogOff;
VOID EFIAPI RkLogSetFramebuffer (UINT64 Base, UINT32 Width, UINT32 Height, UINT32 Stride, UINT32 Bpp)
{
  mLogOff = (Width == 0) || (Height == 0) || (Bpp == 0);
}

EFI_STATUS EFIAPI RkGetBootTags (OUT RK_BOOT_TAGS *Tags)
{
  mInfoCalls++;
  memset (Tags, 0, sizeof (*Tags));
  if (EFI_ERROR (mTagStatus)) {
    return mTagStatus;
  }

  *Tags = mTags;
  return EFI_SUCCESS;
}

RETURN_STATUS EFIAPI AsciiStrCpyS (OUT CHAR8 *Dest, IN UINTN DestMax, IN CONST CHAR8 *Src)
{
  if (strlen (Src) >= DestMax) {
    printf ("FAIL: region name '%s' does not fit %lu bytes\n", Src, (unsigned long)DestMax);
    g_fails++;
    return RETURN_BUFFER_TOO_SMALL;
  }

  strcpy (Dest, Src);
  return RETURN_SUCCESS;
}

#include "MemoryMapLib.c"

// ---- the rules ------------------------------------------------------------------

#define BL31_END      0x00110000ULL    // BL31 + its shared memory (PLAN)
#define RAMOOPS_BASE  0x00110000ULL    // kernel DTS ramoops@110000, 0xf0000
#define RAMOOPS_END   0x00200000ULL
#define LOG_BASE      0x01E00000ULL    // UEFI log, 1 MB (PLAN)
#define LOG_END       0x01F00000ULL
#define FDTWIN_BASE   0x01F00000ULL    // U-Boot's fdt_addr_r (odroidgoa.h) up to the FD
#define FDTWIN_END    0x02000000ULL
#define FD_BASE       0x02000000ULL    // r36s.toml / PLAN: FD 0x02000000 + 0xFF000
#define FD_END        0x020FF000ULL
#define HEAP_SIZE     (80ULL << 20)    // PLAN: DXE heap 80 MB
#define TEE_BASE      0x08400000ULL    // trust.img: BL32 LoadAddr 0x08400000
#define TEE_SIZE      0x01E00000ULL    // BL32 writes 30 MB in its tag and tos_param; kernel logs: 32 MB short
#define MMIO_BASE     0xFF000000ULL    // AGENT-BRIEF: peripherals 0xFF000000-0xFFFFFFFF
#define MMIO_SIZE     0x01000000ULL
#define STACK_BASE    ((UINT64)_PCD_VALUE_PcdCPUCoresStackBase)     // r36s.dsc
#define STACK_END     (STACK_BASE + _PCD_VALUE_PcdCPUCorePrimaryStackSize)
#define MB            (1ULL << 20)

typedef struct {
  CONST char  *Name;
  UINT64      Dram;                 // RkGetDramSize
  UINT64      FbBase;               // 0 = no live framebuffer
  UINT32      FbW, FbH, FbStride, FbBpp;
  BOOLEAN     Tag;                  // RkGetBootTags finds a tag list
  UINT64      TagTee;
  UINT32      TagTeeSize;
  // expected
  UINT64      RamTop;
  BOOLEAN     FbKept;
  UINT64      Tee;
  UINT64      TeeSize;
  CONST char  *KnownId;             // a known firmware bug this case shows ...
  CONST char  *KnownCats;           // ... in these check categories
} CASE;

static CONST CASE  *mCase;

static BOOLEAN InCats (CONST char *Cat)
{
  CONST char  *P;
  size_t      L = strlen (Cat);

  if ((mCase->KnownCats == NULL) || (mCase->KnownId == NULL)) {
    return FALSE;
  }

  for (P = mCase->KnownCats; (P = strstr (P, Cat)) != NULL; P += L) {
    if (((P == mCase->KnownCats) || (P[-1] == ',')) && ((P[L] == ',') || (P[L] == '\0'))) {
      return TRUE;
    }
  }

  return FALSE;
}

#define EXPECT(cat, cond, ...) do { if (InCats (cat)) { KNOWN (cond, mCase->KnownId, __VA_ARGS__); } \
  else { CHECK (cond); if (!(cond)) { printf ("    [%s] ", mCase->Name); printf (__VA_ARGS__); printf ("\n"); } } } while (0)

static void Dump (EFI_MEMORY_REGION_DESCRIPTOR *M, UINT8 N)
{
  UINT8  i;

  for (i = 0; i < N; i++) {
    printf ("      %-18s 0x%09llx-0x%09llx type %2u res %u arm %u\n", M[i].Name, (unsigned long long)M[i].Address,
            (unsigned long long)(M[i].Address + M[i].Length), M[i].MemoryType, M[i].ResourceType, M[i].ArmAttributes);
  }
}

// The region holding Addr, or NULL.
static EFI_MEMORY_REGION_DESCRIPTOR *At (EFI_MEMORY_REGION_DESCRIPTOR *M, UINT8 N, UINT64 Addr)
{
  UINT8  i;

  for (i = 0; i < N; i++) {
    if ((Addr >= M[i].Address) && (Addr < M[i].Address + M[i].Length)) {
      return &M[i];
    }
  }

  return NULL;
}

// Every byte of [Base, End) lies in regions of the given kind:
// 0 = anything but free RAM, 1 = reserved (EfiReservedMemoryType).
static BOOLEAN Covered (EFI_MEMORY_REGION_DESCRIPTOR *M, UINT8 N, UINT64 Base, UINT64 End, int Reserved)
{
  while (Base < End) {
    EFI_MEMORY_REGION_DESCRIPTOR  *R = At (M, N, Base);

    if ((R == NULL) || (R->MemoryType == EfiConventionalMemory) || (Reserved && (R->MemoryType != EfiReservedMemoryType))) {
      return FALSE;
    }

    Base = R->Address + R->Length;
  }

  return TRUE;
}

// What Silicium's SEC looks up (MemoryMapHelperLib: case-insensitive name).
static EFI_MEMORY_REGION_DESCRIPTOR *ByName (EFI_MEMORY_REGION_DESCRIPTOR *M, UINT8 N, CONST char *Name)
{
  UINT8  i;

  for (i = 0; i < N; i++) {
    if (strcasecmp (M[i].Name, Name) == 0) {
      return &M[i];
    }
  }

  return NULL;
}

static void RunCase (CONST CASE *C)
{
  EFI_MEMORY_REGION_DESCRIPTOR  *M, *R;
  UINT8                         N, i;
  UINT64                        Next, Sum, FbStart = 0, FbEnd = 0;
  int                           Fails = g_fails, Known = g_known;

  mCase      = C;
  mDram      = C->Dram;
  mHaveFb    = C->FbBase != 0;
  mTagStatus = C->Tag ? EFI_SUCCESS : EFI_NOT_FOUND;
  mLogOff    = FALSE;
  memset (&mFb, 0, sizeof (mFb));
  memset (&mTags, 0, sizeof (mTags));
  if (mHaveFb) {
    mFb.Base = C->FbBase; mFb.Width = C->FbW; mFb.Height = C->FbH; mFb.Stride = C->FbStride;
    mFb.BitsPerPixel = C->FbBpp; mFb.VopBase = 0xFF460000;
    FbStart = C->FbBase & ~0xFFFULL;
    FbEnd   = (C->FbBase + (UINT64)C->FbStride * C->FbH + 0xFFF) & ~0xFFFULL;
  }

  mTags.TeeBase = C->TagTee;
  mTags.TeeSize = C->TagTeeSize;

  // A fresh start: the library keeps the map it built in module globals.
  mCount = 0;
  memset (mMap, 0xA5, sizeof (mMap));
  GetMemoryMap (&M, &N);

  EXPECT ("count", (N >= 9) && (N <= ARRAY_SIZE (mMap)), "%u regions", N);

  // Each region: a name that fits, a length, whole pages.
  for (i = 0; i < N; i++) {
    EXPECT ("name", memchr (M[i].Name, 0, sizeof (M[i].Name)) != NULL && M[i].Name[0] != 0, "region %u has no name", i);
    EXPECT ("aligned", (M[i].Length != 0) && (((M[i].Address | M[i].Length) & 0xFFF) == 0),
            "'%s' 0x%llx+0x%llx is not whole 4 KB pages", M[i].Name, (unsigned long long)M[i].Address,
            (unsigned long long)M[i].Length);
  }

  // Sorted and no overlaps (Silicium's ValidateMemoryMap dead-loops on one).
  for (i = 0; i + 1 < N; i++) {
    EXPECT ("sorted", M[i].Address + M[i].Length <= M[i + 1].Address, "'%s' ends 0x%llx, '%s' starts 0x%llx",
            M[i].Name, (unsigned long long)(M[i].Address + M[i].Length), M[i + 1].Name, (unsigned long long)M[i + 1].Address);
  }

  // RAM: from 0 to the top without a gap, nothing beyond, all accounted for.
  Next = 0;
  Sum  = 0;
  for (i = 0; i < N; i++) {
    if (M[i].ResourceType == EFI_RESOURCE_MEMORY_MAPPED_IO) {
      continue;
    }

    EXPECT ("ram-top", M[i].Address == Next, "gap or overlap before '%s' at 0x%llx (expected 0x%llx)", M[i].Name,
            (unsigned long long)M[i].Address, (unsigned long long)Next);
    Next = M[i].Address + M[i].Length;
    Sum += M[i].Length;
  }

  EXPECT ("ram-top", Next == C->RamTop, "RAM ends at 0x%llx, the board has 0x%llx", (unsigned long long)Next,
          (unsigned long long)C->RamTop);
  EXPECT ("ram-top", Sum == C->RamTop, "%llu MB of RAM described, the board has %llu MB", (unsigned long long)(Sum / MB),
          (unsigned long long)(C->RamTop / MB));

  // The registers: one device region, nothing of RAM in it.
  R = ByName (M, N, "Peripherals");
  CHECK (R != NULL);
  if (R != NULL) {
    CHECK_EQ (R->Address, MMIO_BASE);
    CHECK_EQ (R->Length, MMIO_SIZE);
    CHECK_EQ (R->ResourceType, EFI_RESOURCE_MEMORY_MAPPED_IO);
    CHECK_EQ (R->MemoryType, EfiMemoryMappedIO);
    CHECK_EQ (R->ArmAttributes, ARM_MEMORY_REGION_ATTRIBUTE_DEVICE);
    CHECK_EQ (R->HobOption, AddDev);
  }

  EXPECT ("mmio", Next <= MMIO_BASE, "RAM runs to 0x%llx, into the registers at 0x%llx", (unsigned long long)Next,
          (unsigned long long)MMIO_BASE);

  // What must survive or stay ours.
  EXPECT ("protect", Covered (M, N, 0, BL31_END, 1), "BL31 0-0x110000 not reserved");
  EXPECT ("protect", Covered (M, N, RAMOOPS_BASE, RAMOOPS_END, 1), "ramoops 0x110000-0x200000 not reserved");
  EXPECT ("protect", Covered (M, N, LOG_BASE, LOG_END, 0), "UEFI log is free RAM");
  EXPECT ("protect", Covered (M, N, FDTWIN_BASE, FDTWIN_END, 0), "FDT window is free RAM");
  EXPECT ("protect", Covered (M, N, FD_BASE, FD_END, 0), "FD is free RAM");
  EXPECT ("protect", Covered (M, N, STACK_BASE, STACK_END, 0), "stack is free RAM");

  // The DXE heap SEC hands to the HOB list (Sec.c looks it up by name).
  R = ByName (M, N, "DXE Heap");
  EXPECT ("heap", R != NULL, "no \"DXE Heap\" region");
  if (R != NULL) {
    EXPECT ("heap", (R->MemoryType == EfiConventionalMemory) && (R->Length == HEAP_SIZE) && (R->Address >= STACK_END) &&
            (R->Address + R->Length <= C->RamTop), "DXE heap 0x%llx+0x%llx type %u", (unsigned long long)R->Address,
            (unsigned long long)R->Length, R->MemoryType);
  }

  // OP-TEE.
  EXPECT ("tee", Covered (M, N, C->Tee, C->Tee + C->TeeSize, 1), "OP-TEE 0x%llx+0x%llx not reserved",
          (unsigned long long)C->Tee, (unsigned long long)C->TeeSize);
  for (i = 0; i < N; i++) {
    if (strcmp (M[i].Name, "OP-TEE") == 0) {
      EXPECT ("tee", (M[i].Address == C->Tee) && (M[i].Length == C->TeeSize), "OP-TEE region 0x%llx+0x%llx, want 0x%llx+0x%llx",
              (unsigned long long)M[i].Address, (unsigned long long)M[i].Length, (unsigned long long)C->Tee,
              (unsigned long long)C->TeeSize);
    }
  }

  // U-Boot's framebuffer: reserved, uncached for the CPU side (the VOP reads
  // DRAM), under the name Silicium's SEC clears and SimpleFbDxe uses.
  R = ByName (M, N, "Display Reserved");
  EXPECT ("fb", (R != NULL) == C->FbKept, "framebuffer %s", C->FbKept ? "not reserved" : "reserved but should not be");
  if ((R != NULL) && C->FbKept) {
    EXPECT ("fb", (R->Address <= C->FbBase) && (R->Address + R->Length >= C->FbBase + (UINT64)C->FbStride * C->FbH),
            "Display Reserved 0x%llx+0x%llx does not hold the framebuffer", (unsigned long long)R->Address,
            (unsigned long long)R->Length);
    EXPECT ("fb", (R->MemoryType == EfiReservedMemoryType) && (R->ArmAttributes != ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK),
            "Display Reserved type %u arm %u", R->MemoryType, R->ArmAttributes);
  }

  // The on-screen log draws on any live 16/32 bpp framebuffer the VOP shows
  // (RkSerialPortLib, from SEC on); those pages must not be handed out.
  if (mHaveFb && ((C->FbBpp == 32) || (C->FbBpp == 16))) {
    UINT64  B;
    BOOLEAN Free = FALSE;

    for (B = FbStart; B < FbEnd && !Free; B += 0x1000) {
      EFI_MEMORY_REGION_DESCRIPTOR  *P = At (M, N, B);
      Free = (P != NULL) && (P->MemoryType == EfiConventionalMemory);
    }

    EXPECT ("fb-log", !Free || mLogOff, "the screen log draws on 0x%llx-0x%llx, which the map hands out as free RAM",
            (unsigned long long)FbStart, (unsigned long long)FbEnd);
  }

  // Everything else is plain write-back RAM.
  for (i = 0; i < N; i++) {
    if (strcmp (M[i].Name, "RAM Partition") == 0) {
      CHECK ((M[i].MemoryType == EfiConventionalMemory) && (M[i].ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) &&
             (M[i].ArmAttributes == ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK) && (M[i].HobOption == AddMem));
    }
  }

  if ((g_fails != Fails) || ((g_known != Known) && (getenv ("VERBOSE") != NULL))) {
    printf ("  case '%s' (%s): %u regions\n", C->Name, g_fails != Fails ? "FAILED" : "known bug", N);
    Dump (M, N);
  }
}

#define LOGO24(base)  (base), 640, 480, 1920, 24
#define LOGO32(base)  (base), 640, 480, 2560, 32
#define NOFB          0, 0, 0, 0, 0
#define NOTAG         FALSE, 0, 0
#define DEFTEE        TEE_BASE, TEE_SIZE

static CONST CASE  Cases[] = {
  // RAM sizes, U-Boot's logo near the top of RAM (video_reserve).
  { "512 MB, U-Boot logo",            512 * MB,  LOGO24 (0x1DC00000), NOTAG, 512 * MB,  TRUE,  DEFTEE },
  { "512 MB, no screen",              512 * MB,  NOFB,                NOTAG, 512 * MB,  FALSE, DEFTEE },
  { "1 GB, U-Boot logo",              1024 * MB, LOGO24 (0x3DC00000), NOTAG, 1024 * MB, TRUE,  DEFTEE },
  { "2 GB",                           2048 * MB, LOGO24 (0x7DC00000), NOTAG, 2048 * MB, TRUE,  DEFTEE },
  { "768 MB",                         768 * MB,  LOGO24 (0x2DC00000), NOTAG, 768 * MB,  TRUE,  DEFTEE },
  { "1.5 GB",                         1536 * MB, LOGO24 (0x5DC00000), NOTAG, 1536 * MB, TRUE,  DEFTEE },
  { "3 GB",                           3072 * MB, LOGO24 (0xBDC00000), NOTAG, 3072 * MB, TRUE,  DEFTEE },
  { "256 MB",                         256 * MB,  LOGO24 (0x0DC00000), NOTAG, 256 * MB,  TRUE,  DEFTEE },
  { "4080 MB (U-Boot's limit)",       0xFF000000, LOGO24 (0xFDC00000), NOTAG, 0xFF000000, TRUE, DEFTEE },
  // DDR record unreadable: 512 MB, or 1 GB when the logo shows there is more.
  { "no DDR record, logo low",        0,         LOGO24 (0x1DC00000), NOTAG, 512 * MB,  TRUE,  DEFTEE },
  { "no DDR record, logo high",       0,         LOGO24 (0x3DC00000), NOTAG, 1024 * MB, TRUE,  DEFTEE },
  { "no DDR record, no screen",       0,         NOFB,                NOTAG, 512 * MB,  FALSE, DEFTEE },
  // Framebuffer shapes.
  { "32 bpp logo",                    512 * MB,  LOGO32 (0x1DC00000), NOTAG, 512 * MB,  TRUE,  DEFTEE },
  { "16 bpp logo",                    512 * MB,  0x1DC00000, 640, 480, 1280, 16, NOTAG, 512 * MB, TRUE, DEFTEE },
  { "logo ends at the top of RAM",    512 * MB,  LOGO32 (0x20000000 - 640 * 480 * 4), NOTAG, 512 * MB, TRUE, DEFTEE },
  { "logo at an odd address",         512 * MB,  0x1DC00800, 641, 479, 1924, 24, NOTAG, 512 * MB, TRUE, DEFTEE },
  { "widest stride",                  512 * MB,  0x1D000000, 640, 480, 0x1FFF * 4, 32, NOTAG, 512 * MB, TRUE, DEFTEE },
  { "logo right above OP-TEE",        512 * MB,  LOGO32 (0x0A200000), NOTAG, 512 * MB, TRUE,  DEFTEE },
  // Framebuffers the map cannot keep.
  { "logo above the RAM",             512 * MB,  LOGO24 (0x30000000), NOTAG, 512 * MB, FALSE, DEFTEE },
  { "logo across the top of RAM",     512 * MB,  LOGO32 (0x1FF80000), NOTAG, 512 * MB, FALSE, DEFTEE },
  { "logo inside the DXE heap",       512 * MB,  LOGO32 (0x04000000), NOTAG, 512 * MB, FALSE, DEFTEE },
  { "logo between heap and OP-TEE",   512 * MB,  LOGO32 (0x08000000), NOTAG, 512 * MB, FALSE, DEFTEE },
  { "logo on OP-TEE",                 512 * MB,  LOGO32 (0x08800000), NOTAG, 512 * MB, FALSE, DEFTEE },
  { "logo in low RAM",                512 * MB,  LOGO32 (0x00400000), NOTAG, 512 * MB, FALSE, DEFTEE },
  { "logo on BL31/ramoops",           512 * MB,  LOGO32 (0x00100000), NOTAG, 512 * MB, FALSE, DEFTEE },
  { "logo 24 bpp in the heap",        512 * MB,  LOGO24 (0x04000000), NOTAG, 512 * MB, FALSE, DEFTEE },
  // Miniloader tags.
  { "tag: OP-TEE as trust.img",       512 * MB,  LOGO24 (0x1DC00000), TRUE, TEE_BASE, TEE_SIZE, 512 * MB, TRUE, DEFTEE },
  { "tag: OP-TEE 32 MB at 256 MB",    512 * MB,  LOGO24 (0x1DC00000), TRUE, 0x10000000, 32 * MB, 512 * MB, TRUE, 0x10000000, 32 * MB },
  { "tag: OP-TEE under the heap end", 512 * MB,  LOGO24 (0x1DC00000), TRUE, 0x05000000, 16 * MB, 512 * MB, TRUE, DEFTEE },
  { "tag: OP-TEE across 512 MB",      512 * MB,  LOGO24 (0x1DC00000), TRUE, 0x1F800000, 16 * MB, 512 * MB, TRUE, DEFTEE },
  { "tag: no OP-TEE in it",           512 * MB,  LOGO24 (0x1DC00000), TRUE, 0, 0, 512 * MB, TRUE, DEFTEE },
  { "tag: OP-TEE over the logo",      512 * MB,  LOGO24 (0x1DC00000), TRUE, 0x1D000000, 32 * MB, 512 * MB, FALSE, 0x1D000000, 32 * MB },
  { "tag: OP-TEE 1 MB + 2 KB",        512 * MB,  LOGO24 (0x1DC00000), TRUE, TEE_BASE, SIZE_1MB + 0x800, 512 * MB, TRUE,
    DEFTEE },
  { "tag: OP-TEE above 512 MB, 1 GB", 1024 * MB, LOGO24 (0x3DC00000), TRUE, 0x30000000, 16 * MB, 1024 * MB, TRUE,
    0x30000000, 16 * MB },
  // Records the decoder accepts but no console has.
  { "128 MB",                         128 * MB,  NOFB, NOTAG, 128 * MB, FALSE, DEFTEE, "dram-128m", "ram-top" },
  { "4 GB record",                    SIZE_4GB,  LOGO24 (0xFDC00000), NOTAG, 0xFF000000, TRUE, DEFTEE },
};

int main (void)
{
  EFI_MEMORY_REGION_DESCRIPTOR  *M, *M2, *R;
  UINT8                         N, N2;
  UINTN                         i;
  int                           Calls;

  // The build's layout is the one PLAN.md describes.
  CHECK_EQ (_PCD_VALUE_PcdFdBaseAddress, FD_BASE);
  CHECK_EQ (_PCD_VALUE_PcdFdBaseAddress + _PCD_VALUE_PcdFdSize, FD_END);
  CHECK (STACK_BASE >= FD_END);
  CHECK_EQ (RK_LOG_BASE, LOG_BASE);
  CHECK_EQ (RK_LOG_BASE + RK_LOG_SIZE, LOG_END);
  CHECK_EQ (RK_BOOTLOADER_FDT_BASE, FDTWIN_BASE);
  CHECK_EQ (RK_BOOTLOADER_FDT_BASE + RK_BOOTLOADER_FDT_SIZE, FDTWIN_END);

  for (i = 0; i < ARRAY_SIZE (Cases); i++) {
    RunCase (&Cases[i]);
  }

  printf ("%lu cases\n", (unsigned long)ARRAY_SIZE (Cases));

  // Built once per start: a second call hands back the same map without
  // asking the hardware again (SEC, PEI and DXE all call it).
  mCase = &Cases[0];
  RunCase (&Cases[0]);
  GetMemoryMap (&M, &N);
  Calls = mInfoCalls;
  mDram = 2048 * MB;
  mHaveFb = FALSE;
  GetMemoryMap (&M2, &N2);
  CHECK (M2 == M);
  CHECK_EQ (N2, N);
  CHECK_EQ (mInfoCalls, Calls);

  // The log ring must reach DRAM before a reset: the previous-boot log is
  // read after a hang and reset, when the caches have been dropped without
  // being written back. A write-back mapping keeps the newest lines (and the
  // ring header with the byte count) in the cache.
  R = ByName (M, N, "UEFI Log");
  CHECK (R != NULL);
  if (R != NULL) {
    CHECK (R->ArmAttributes == ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED);
  }

  DONE_KNOWN ();
}

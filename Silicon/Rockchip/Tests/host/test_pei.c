// PlatformPeiLib: what SEC hands to DXE in the board facts HOB, and the rule
// for the FDT pointer in x0. x0 is only an FDT when U-Boot's booti started
// us; it is trusted only inside the window the memory map keeps for it
// (PLAN.md: 0x01F00000-0x02000000, U-Boot's fdt_addr_r in odroidgoa.h),
// 8-byte aligned (Linux arm64 booting.rst: "The device tree blob (dtb) must
// be placed on an 8-byte boundary"), with the FDT magic and a totalsize that
// fits the window (devicetree spec 5.2: big-endian header, magic 0xd00dfeed).
// Only the window is mapped, with no-access pages on both sides, so reading
// through a hostile x0 crashes the test.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent

#define _GNU_SOURCE
#include <sys/mman.h>
#include <PiPei.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/RkPlatformInfoLib.h>
#include <Guid/RkPlatformInfoHob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"

// ---- RkPlatformInfoLib as each case sets it ------------------------------------------

static UINT64               mDram, mX0;
static BOOLEAN              mFbOn, mTagsOn;
static RK_FRAMEBUFFER_INFO  mFb;
static RK_BOOT_TAGS         mTags;

UINT64 EFIAPI RkGetDramSize (VOID) { return mDram; }
EFI_PHYSICAL_ADDRESS EFIAPI RkGetBootloaderFdt (VOID) { return mX0; }

EFI_STATUS EFIAPI RkGetActiveFramebuffer (OUT RK_FRAMEBUFFER_INFO *Info)
{
  if (!mFbOn) {
    memset (Info, 0x5A, sizeof (*Info));      // the real one may leave partial values
    return EFI_NOT_FOUND;
  }

  *Info = mFb;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI RkGetBootTags (OUT RK_BOOT_TAGS *Tags)
{
  memset (Tags, 0, sizeof (*Tags));           // RkPlatformInfoLib.h: fields without a tag stay 0
  if (!mTagsOn) {
    return EFI_NOT_FOUND;
  }

  *Tags = mTags;
  return EFI_SUCCESS;
}

// ---- HOBs, CPU --------------------------------------------------------------------------

EFI_GUID  gRkPlatformInfoHobGuid = { 0x6a3b1c9e, 0x4f7d, 0x4b52, { 0x9a, 0x1e, 0x2c, 0x3d, 0x4e, 0x5f, 0x60, 0x71 } };

static int                   nFv, nGuid;
static UINT64                FvBase, FvLen;
static RK_PLATFORM_INFO_HOB  Hob;
static UINTN                 HobLen;

VOID EFIAPI BuildFvHob (IN EFI_PHYSICAL_ADDRESS BaseAddress, IN UINT64 Length) { nFv++; FvBase = BaseAddress; FvLen = Length; }

VOID * EFIAPI BuildGuidDataHob (IN CONST EFI_GUID *Guid, IN VOID *Data, IN UINTN DataLength)
{
  nGuid++;
  CHECK (memcmp (Guid, &gRkPlatformInfoHobGuid, sizeof (*Guid)) == 0);
  HobLen = DataLength;
  memcpy (&Hob, Data, MIN (DataLength, sizeof (Hob)));
  return &Hob;
}

UINTN EFIAPI ArmReadCntFrq (VOID) { return 24000000; }
UINT32 EFIAPI SwapBytes32 (IN UINT32 V) { return __builtin_bswap32 (V); }

// r36s.fdf: the FV is the whole FD (the Makefile passes r36s.toml's values).
#ifndef _PCD_GET_MODE_64_PcdFvBaseAddress
#define _PCD_GET_MODE_64_PcdFvBaseAddress  0x02000000ULL
#define _PCD_GET_MODE_32_PcdFvSize         0x000FF000U
#endif

// The one AArch64 instruction (mrs CurrentEL) cannot build for the host: it
// reads EL2 here.
#define __asm__
#define volatile(...)  (El = 2 << 2)
#include "PlatformPeiLib.c"
#undef volatile
#undef __asm__

// ---- cases ---------------------------------------------------------------------------------

#define WIN      0x01F00000ULL
#define WIN_END  0x02000000ULL

static void PutFdt (UINT64 At, UINT32 Magic, UINT32 TotalSize)
{
  UINT32  *H = (UINT32 *)(UINTN)At;

  H[0] = __builtin_bswap32 (Magic);       // big-endian header
  H[1] = __builtin_bswap32 (TotalSize);
}

static void ExpectFdt (CONST char *What, UINT64 X0, UINT64 WantBase, UINT32 WantSize)
{
  mX0 = X0;
  nGuid = 0;
  memset (&Hob, 0xEE, sizeof (Hob));
  CHECK_EQ (PlatformPeim (), EFI_SUCCESS);
  CHECK_EQ (nGuid, 1);
  if ((Hob.BootloaderFdt != WantBase) || (Hob.BootloaderFdtSize != WantSize)) {
    printf ("FAIL: x0 '%s' 0x%llx: HOB FDT 0x%llx size %u, want 0x%llx size %u\n", What, (unsigned long long)X0,
            (unsigned long long)Hob.BootloaderFdt, Hob.BootloaderFdtSize, (unsigned long long)WantBase, WantSize);
  }

  CHECK_EQ (Hob.BootloaderFdt, WantBase);
  CHECK_EQ (Hob.BootloaderFdtSize, WantSize);
}

int main (void)
{
  void  *Lo, *Win, *Hi;

  Lo  = mmap ((void *)(WIN - 0x1000), 0x1000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  Win = mmap ((void *)WIN, WIN_END - WIN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  Hi  = mmap ((void *)WIN_END, 0x1000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if ((Lo != (void *)(WIN - 0x1000)) || (Win != (void *)WIN) || (Hi != (void *)WIN_END)) {
    printf ("FAIL: cannot map the FDT window on this host\n");
    return 1;
  }

  CHECK_EQ (RK_BOOTLOADER_FDT_BASE, WIN);
  CHECK_EQ (RK_BOOTLOADER_FDT_BASE + RK_BOOTLOADER_FDT_SIZE, WIN_END);

  // The facts as RkPlatformInfoLib reported them.
  mDram   = 1ULL << 30;
  mFbOn   = TRUE;
  mFb.Base = 0x3DC00000; mFb.Width = 640; mFb.Height = 480; mFb.Stride = 1920; mFb.BitsPerPixel = 24;
  mFb.VopBase = 0xFF460000;
  mTagsOn = TRUE;
  mTags.BootDevType = BIT2; mTags.BootDevNum = 0; mTags.TeeBase = 0x08400000; mTags.TeeSize = 0x01000000;
  PutFdt (WIN, 0xD00DFEED, 0x1A2B4);
  ExpectFdt ("U-Boot's booti", WIN, WIN, 0x1A2B4);
  CHECK_EQ (nFv, 1);
  CHECK_EQ (FvBase, _PCD_GET_MODE_64_PcdFvBaseAddress);
  CHECK_EQ (FvLen, _PCD_GET_MODE_32_PcdFvSize);
  CHECK_EQ (HobLen, sizeof (RK_PLATFORM_INFO_HOB));
  CHECK_EQ (Hob.Version, RK_PLATFORM_INFO_HOB_VERSION);
  CHECK (Hob.Version >= 2);                         // BootTags are a version 2 field
  CHECK_EQ (Hob.DramSize, 1ULL << 30);
  CHECK_EQ (Hob.FramebufferValid, 1);
  CHECK (memcmp (&Hob.Framebuffer, &mFb, sizeof (mFb)) == 0);
  CHECK (memcmp (&Hob.BootTags, &mTags, sizeof (mTags)) == 0);

  // No framebuffer, no tags, no DDR record: zeros DXE can recognise.
  mFbOn = FALSE;
  mTagsOn = FALSE;
  mDram = 0;
  ExpectFdt ("no facts", WIN, WIN, 0x1A2B4);
  CHECK_EQ (Hob.FramebufferValid, 0);
  CHECK_EQ (Hob.DramSize, 0);
  CHECK_EQ (Hob.BootTags.BootDevType, 0);
  CHECK_EQ (Hob.BootTags.TeeBase, 0);
  CHECK_EQ (Hob.BootTags.TeeSize, 0);
  CHECK_EQ (Hob.Reserved, 0);

  // Hostile or stale x0 (started by BL31 as uboot.img: whatever it left).
  PutFdt (WIN, 0xD00DFEED, 0x10000);
  ExpectFdt ("0", 0, 0, 0);
  ExpectFdt ("below the window", WIN - 8, 0, 0);
  ExpectFdt ("a page below", WIN - 0x1000, 0, 0);
  ExpectFdt ("the window's end", WIN_END, 0, 0);
  ExpectFdt ("the FD", 0x02000100, 0, 0);
  ExpectFdt ("above 4 GB, window offset", 0x101F00000ULL, 0, 0);
  ExpectFdt ("all ones", ~0ULL, 0, 0);
  ExpectFdt ("all ones, aligned", ~7ULL, 0, 0);
  ExpectFdt ("4 bytes in", WIN + 4, 0, 0);
  ExpectFdt ("1 byte in", WIN + 1, 0, 0);

  // Inside the window but not an FDT.
  memset ((void *)WIN, 0, 64);
  ExpectFdt ("zeros", WIN, 0, 0);
  PutFdt (WIN, 0xEDFE0DD0, 0x10000);                // magic stored little-endian
  ExpectFdt ("byte-swapped magic", WIN, 0, 0);
  PutFdt (WIN, 0xD00DFEEE, 0x10000);
  ExpectFdt ("magic off by one", WIN, 0, 0);

  // Sizes: at least a header, never past the window.
  PutFdt (WIN, 0xD00DFEED, 0);
  ExpectFdt ("size 0", WIN, 0, 0);
  PutFdt (WIN, 0xD00DFEED, 63);
  ExpectFdt ("size 63", WIN, 0, 0);
  PutFdt (WIN, 0xD00DFEED, 64);
  ExpectFdt ("size 64", WIN, WIN, 64);
  PutFdt (WIN, 0xD00DFEED, 0x100000);
  ExpectFdt ("the whole window", WIN, WIN, 0x100000);
  PutFdt (WIN, 0xD00DFEED, 0x100001);
  ExpectFdt ("a byte past the window", WIN, 0, 0);
  PutFdt (WIN, 0xD00DFEED, 0xFFFFFFFF);
  ExpectFdt ("size 0xffffffff", WIN, 0, 0);
  PutFdt (WIN, 0xD00DFEED, 0x80000000);
  ExpectFdt ("size 2 GB", WIN, 0, 0);

  // Near the end of the window.
  PutFdt (WIN_END - 0x100, 0xD00DFEED, 0x100);
  ExpectFdt ("ends at the window's end", WIN_END - 0x100, WIN_END - 0x100, 0x100);
  PutFdt (WIN_END - 0x100, 0xD00DFEED, 0x108);
  ExpectFdt ("8 bytes past", WIN_END - 0x100, 0, 0);
  PutFdt (WIN_END - 8, 0xD00DFEED, 64);
  ExpectFdt ("header in the last 8 bytes", WIN_END - 8, 0, 0);
  PutFdt (WIN_END - 64, 0xD00DFEED, 64);
  ExpectFdt ("64 bytes at the end", WIN_END - 64, WIN_END - 64, 64);

  // Every aligned position with a fitting size, and a size one too big.
  {
    UINT64  At;

    for (At = WIN; At < WIN_END; At += 0x1F8) {
      UINT32  Fit = (UINT32)(WIN_END - At);

      if (Fit < 64) {
        continue;
      }

      PutFdt (At, 0xD00DFEED, Fit);
      mX0 = At;
      PlatformPeim ();
      CHECK (Hob.BootloaderFdt == At && Hob.BootloaderFdtSize == Fit);
      PutFdt (At, 0xD00DFEED, Fit + 1);
      PlatformPeim ();
      CHECK (Hob.BootloaderFdt == 0 && Hob.BootloaderFdtSize == 0);
    }
  }

  DONE ();
}

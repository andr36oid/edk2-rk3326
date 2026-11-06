/** @file
  Host tests for LinuxBootLib's pure parts (LinuxImage.c, LinuxFdt.c).
  Usage: TestLinuxBoot <kernel-test.dtb> <handover-test.dtb> <outdir> [real.dtb]
  Writes fixed-up DTBs to outdir for run.sh to check with fdtget, which
  reads them with a different libfdt than the one under test.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/FdtLib.h>
#include <Library/LinuxBootLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

STATIC int  mFailed;
STATIC int  mPassed;

#define CHECK(Cond)                                                   \
  do {                                                                \
    if (Cond) { mPassed++; }                                          \
    else { mFailed++; fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); } \
  } while (0)

STATIC
VOID *
ReadFile (
  CONST char  *Path,
  UINTN       *Size
  )
{
  FILE  *F = fopen (Path, "rb");
  VOID  *B;
  long  L;

  if (F == NULL) {
    return NULL;
  }

  fseek (F, 0, SEEK_END);
  L = ftell (F);
  fseek (F, 0, SEEK_SET);
  B = malloc (L);
  if (fread (B, 1, L, F) != (size_t)L) {
    exit (2);
  }

  fclose (F);
  *Size = L;
  return B;
}

STATIC
VOID
WriteFile (
  CONST char  *Dir,
  CONST char  *Name,
  CONST VOID  *B,
  UINTN       Size
  )
{
  char  Path[512];
  FILE  *F;

  snprintf (Path, sizeof (Path), "%s/%s", Dir, Name);
  F = fopen (Path, "wb");
  fwrite (B, 1, Size, F);
  fclose (F);
}

STATIC
VOID
PutLe64 (
  UINT8   *P,
  UINT64  V
  )
{
  int  I;

  for (I = 0; I < 8; I++) {
    P[I] = (UINT8)(V >> (8 * I));
  }
}

STATIC
VOID
PutBe32 (
  UINT8   *P,
  UINT32  V
  )
{
  P[0] = (UINT8)(V >> 24);
  P[1] = (UINT8)(V >> 16);
  P[2] = (UINT8)(V >> 8);
  P[3] = (UINT8)V;
}

STATIC
VOID
TestImageHeader (
  VOID
  )
{
  UINT8                   Img[4096];
  LINUX_ARM64_IMAGE_INFO  Info;

  memset (Img, 0, sizeof (Img));
  PutLe64 (Img + 8, 0x80000);
  PutLe64 (Img + 16, 0x2000000);
  PutLe64 (Img + 24, 0xA);        // LE, 4K pages, place anywhere
  PutLe64 (Img + 56, LINUX_ARM64_MAGIC);
  CHECK (LinuxArm64ParseHeader (Img, sizeof (Img), &Info) == RETURN_SUCCESS);
  CHECK (Info.TextOffset == 0x80000);
  CHECK (Info.ImageSize == 0x2000000);
  CHECK (Info.Flags == 0xA);

  // Old kernels: image_size 0 means text_offset 0x80000 and the file size.
  PutLe64 (Img + 8, 0x12345);
  PutLe64 (Img + 16, 0);
  CHECK (LinuxArm64ParseHeader (Img, sizeof (Img), &Info) == RETURN_SUCCESS);
  CHECK (Info.TextOffset == 0x80000);
  CHECK (Info.ImageSize == sizeof (Img));

  // image_size smaller than the file: the file wins.
  PutLe64 (Img + 8, 0);
  PutLe64 (Img + 16, 16);
  CHECK (LinuxArm64ParseHeader (Img, sizeof (Img), &Info) == RETURN_SUCCESS);
  CHECK (Info.ImageSize == sizeof (Img));

  PutLe64 (Img + 8, 0x200000);    // text_offset must stay below 2 MB
  PutLe64 (Img + 16, 0x100000);
  CHECK (LinuxArm64ParseHeader (Img, sizeof (Img), &Info) == RETURN_UNSUPPORTED);

  PutLe64 (Img + 8, 0x80000);
  PutLe64 (Img + 24, 0xB);        // big-endian kernel
  CHECK (LinuxArm64ParseHeader (Img, sizeof (Img), &Info) == RETURN_UNSUPPORTED);

  PutLe64 (Img + 24, 0xA);
  Img[56] = 'X';
  CHECK (LinuxArm64ParseHeader (Img, sizeof (Img), &Info) == RETURN_UNSUPPORTED);
  CHECK (LinuxArm64ParseHeader (Img, 32, &Info) == RETURN_INVALID_PARAMETER);
}

STATIC
VOID
TestUImage (
  VOID
  )
{
  UINT8  U[200];
  UINTN  Off;
  UINTN  Size;

  memset (U, 0, sizeof (U));
  PutBe32 (U, LINUX_UIMAGE_MAGIC);
  PutBe32 (U + 12, 100);
  U[28] = 5;    // linux
  U[29] = 22;   // arm64
  U[30] = 3;    // ramdisk
  U[31] = 0;    // no compression
  CHECK (LinuxUImagePayload (U, sizeof (U), &Off, &Size) == RETURN_SUCCESS);
  CHECK (Off == 64);
  CHECK (Size == 100);

  CHECK (LinuxUImagePayload (U, 100, &Off, &Size) == RETURN_VOLUME_CORRUPTED);  // short
  U[31] = 1;    // gzip: booti would not take it either
  CHECK (LinuxUImagePayload (U, sizeof (U), &Off, &Size) == RETURN_VOLUME_CORRUPTED);
  U[31] = 0;
  U[30] = 2;    // kernel
  CHECK (LinuxUImagePayload (U, sizeof (U), &Off, &Size) == RETURN_VOLUME_CORRUPTED);
  U[0] = 0x1F;  // a plain gzip'd cpio is used as it is
  CHECK (LinuxUImagePayload (U, sizeof (U), &Off, &Size) == RETURN_UNSUPPORTED);
}

STATIC
VOID
TestFdt (
  CONST char  *KernelDtb,
  CONST char  *HandoverDtb,
  CONST char  *OutDir
  )
{
  VOID                *Src;
  VOID                *Handover;
  UINTN               SrcSize;
  UINTN               HandoverSize;
  UINT8               Buf[65536];
  LINUX_MEMORY_RANGE  Mem[2] = {
    { 0x00200000, 0x08200000 },
    { 0x09400000, 0x36C00000 }
  };
  LINUX_MEMORY_RANGE  Got[8];
  LINUX_FDT_FIXUP     Fix;
  CONST CHAR8         *Name;
  CONST CHAR8         *Args;

  Src      = ReadFile (KernelDtb, &SrcSize);
  Handover = ReadFile (HandoverDtb, &HandoverSize);
  CHECK (Src != NULL && Handover != NULL);
  if ((Src == NULL) || (Handover == NULL)) {
    return;
  }

  // What U-Boot tells us on its command line, and its RAM banks.
  Args = LinuxFdtGetBootargs (Handover);
  CHECK (Args != NULL && strstr (Args, "andr36oid.hwrev=rg351mp") != NULL);
  CHECK (LinuxFdtGetMemory (Handover, Got, 8) == 1);
  CHECK (Got[0].Base == 0x200000 && Got[0].Size == 0x3e000000);

  // Full fixup: bootargs, initrd, two RAM banks, logo handed over.
  CHECK (FdtOpenInto (Src, Buf, sizeof (Buf)) == 0);
  memset (&Fix, 0, sizeof (Fix));
  Fix.Bootargs    = "console=null androidboot.hardware=rk30board";
  Fix.InitrdStart = 0x0A200000;
  Fix.InitrdSize  = 0x123456;
  Fix.Memory      = Mem;
  Fix.MemoryCount = 2;
  Fix.HandoverFdt = Handover;
  CHECK (LinuxFdtFixup (Buf, sizeof (Buf), &Fix) == RETURN_SUCCESS);
  WriteFile (OutDir, "fixed-handover.dtb", Buf, FdtTotalSize (Buf));

  CHECK (strcmp (LinuxFdtGetBootargs (Buf), Fix.Bootargs) == 0);
  CHECK (LinuxFdtGetMemory (Buf, Got, 8) == 2);
  CHECK (Got[0].Base == Mem[0].Base && Got[0].Size == Mem[0].Size);
  CHECK (Got[1].Base == Mem[1].Base && Got[1].Size == Mem[1].Size);

  // Placement checks against the fixed-up tree.
  CHECK (LinuxFdtRangeConflicts (Buf, Mem, 2, 0x00100000, 0x20000, &Name));  // below RAM
  CHECK (Name != NULL && strcmp (Name, "outside /memory") == 0);
  CHECK (LinuxFdtRangeConflicts (Buf, NULL, 0, 0x00180000, 0x1000, &Name)); // ramoops
  CHECK (Name != NULL && strcmp (Name, "ramoops@110000") == 0);
  CHECK (LinuxFdtRangeConflicts (Buf, NULL, 0, 0x3E000000, 0x1000, &Name)); // logo region
  CHECK (Name != NULL && strncmp (Name, "drm-logo", 8) == 0);
  CHECK (!LinuxFdtRangeConflicts (Buf, Mem, 2, 0x10000000, 0x2000000, &Name));
  CHECK (LinuxFdtRangeConflicts (Buf, Mem, 2, 0x08300000, 0x2000000, &Name)); // OP-TEE hole

  // No handover FDT, live framebuffer only: the region is filled, nothing else.
  CHECK (FdtOpenInto (Src, Buf, sizeof (Buf)) == 0);
  memset (&Fix, 0, sizeof (Fix));
  Fix.Bootargs = "a b c";
  Fix.FbBase   = 0x3F000000;
  Fix.FbSize   = 0x12C000;
  CHECK (LinuxFdtFixup (Buf, sizeof (Buf), &Fix) == RETURN_SUCCESS);
  WriteFile (OutDir, "fixed-fb.dtb", Buf, FdtTotalSize (Buf));
  CHECK (LinuxFdtGetMemory (Buf, Got, 8) == 2);   // MemoryCount 0 keeps the DTB's

  // A buffer with no room fails cleanly instead of corrupting.
  CHECK (FdtOpenInto (Src, Buf, (INT32)SrcSize) == 0);
  memset (&Fix, 0, sizeof (Fix));
  Fix.Bootargs = "x";
  {
    static char  Long[8192];
    memset (Long, 'a', sizeof (Long) - 1);
    Fix.Bootargs = Long;
  }
  CHECK (LinuxFdtFixup (Buf, SrcSize, &Fix) == RETURN_BUFFER_TOO_SMALL);
}

STATIC
VOID
TestRealDtb (
  CONST char  *Path,
  CONST char  *HandoverDtb,
  CONST char  *OutDir
  )
{
  VOID                *Src;
  VOID                *Handover;
  UINTN               Size;
  UINTN               HSize;
  static UINT8        Buf[512 * 1024];
  LINUX_MEMORY_RANGE  Mem[1] = {
    { 0x00200000, 0x3FE00000 }
  };
  LINUX_FDT_FIXUP     Fix;

  Src      = ReadFile (Path, &Size);
  Handover = ReadFile (HandoverDtb, &HSize);
  if (Src == NULL) {
    printf ("real DTB %s not found, skipped\n", Path);
    return;
  }

  CHECK (FdtOpenInto (Src, Buf, sizeof (Buf)) == 0);
  memset (&Fix, 0, sizeof (Fix));
  Fix.Bootargs    = "fw_devlink=permissive rootwait androidboot.revision=rg351mp";
  Fix.InitrdStart = 0x0A200000;
  Fix.InitrdSize  = 0x400000;
  Fix.Memory      = Mem;
  Fix.MemoryCount = 1;
  Fix.HandoverFdt = Handover;
  CHECK (LinuxFdtFixup (Buf, sizeof (Buf), &Fix) == RETURN_SUCCESS);
  WriteFile (OutDir, "fixed-real.dtb", Buf, FdtTotalSize (Buf));
}

int
main (
  int   argc,
  char  **argv
  )
{
  if (argc < 4) {
    fprintf (stderr, "usage: %s kernel.dtb handover.dtb outdir [real.dtb]\n", argv[0]);
    return 2;
  }

  TestImageHeader ();
  TestUImage ();
  TestFdt (argv[1], argv[2], argv[3]);
  if (argc > 4) {
    TestRealDtb (argv[4], argv[2], argv[3]);
  }

  printf ("TestLinuxBoot: %d passed, %d failed\n", mPassed, mFailed);
  return mFailed ? 1 : 0;
}

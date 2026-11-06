// Host test of RkDisplayDxe's entry point: which FDT it takes, adopt vs
// reinit vs a dark screen, the bootargs keys, which framebuffer the GOP
// gets, and that adopt touches no register. Boot services, the HOB and the
// GOP are faked; the bring-up runs on the fake PX30 of fake_env.c.
//
//   test_entry <android.dtb> <screen.dtb>
//
// SPDX-License-Identifier: BSD-2-Clause-Patent

#define _GNU_SOURCE
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char  mock_pcd_mode;
#define _PCD_VALUE_PcdRkDisplayMode  mock_pcd_mode

#undef NULL
#include "RkDisplayDxe.c"
#include "fake_env.h"
#include "test.h"
#undef FDT_TAGSIZE
#include <libfdt.h>

EFI_GUID  gRkPlatformInfoHobGuid   = { 0x1 };
EFI_GUID  gRkScreenFdtProtocolGuid = { 0x15e48db0, 0xa55a, 0x4e2f, { 0x83, 0xd3, 0x17, 0xbb, 0x5c, 0x9f, 0x91, 0x75 } };

// ---- Fakes -------------------------------------------------------------------

static struct {
  EFI_HOB_GUID_TYPE       Hdr;
  RK_PLATFORM_INFO_HOB    Info;
} mHob;

static BOOLEAN              mHaveHob;
static int                  mGopCalls;
static RK_FRAMEBUFFER_INFO  mGop;
static int                  mPagesCalls;
static EFI_EVENT_NOTIFY     mNotifyFn;
static RK_SCREEN_FDT_PROTOCOL  *mProto;   // what LocateProtocol hands out next

VOID * EFIAPI GetFirstGuidHob (IN CONST EFI_GUID *Guid) { return mHaveHob ? &mHob : NULL; }
VOID * EFIAPI AllocateZeroPool (IN UINTN Size) { return calloc (1, Size); }

static UINT64  mLogFbBase;

VOID EFIAPI
RkLogSetFramebuffer (IN UINT64 Base, IN UINT32 Width, IN UINT32 Height, IN UINT32 Stride, IN UINT32 BitsPerPixel)
{
  mLogFbBase = Base;
}

EFI_STATUS EFIAPI
RkGopInstall (IN CONST RK_FRAMEBUFFER_INFO *Fb, OUT EFI_HANDLE *Handle OPTIONAL)
{
  // The on-screen log must already draw where the GOP points.
  CHECK_EQ (mLogFbBase, Fb->Base);
  if (mGopCalls++ > 0) {
    return EFI_ALREADY_STARTED;
  }

  mGop = *Fb;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeAllocatePages (EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory)
{
  void  *p = mmap (NULL, Pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);

  mPagesCalls++;
  CHECK (Type == AllocateMaxAddress);
  CHECK (MemType == EfiReservedMemoryType);
  if (p == MAP_FAILED) {
    return EFI_OUT_OF_RESOURCES;
  }

  memset (p, 0x5A, Pages * 4096);
  *Memory = (UINTN)p;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeCreateEvent (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Fn, VOID *Ctx, EFI_EVENT *Event)
{
  mNotifyFn = Fn;
  *Event    = (EFI_EVENT)1;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeRegisterProtocolNotify (EFI_GUID *Protocol, EFI_EVENT Event, VOID **Registration)
{
  *Registration = (VOID *)2;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeSignalEvent (EFI_EVENT Event)
{
  if (mNotifyFn != NULL) {
    mNotifyFn (Event, NULL);
  }

  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeLocateProtocol (EFI_GUID *Protocol, VOID *Registration, VOID **Interface)
{
  if (mProto == NULL) {
    return EFI_NOT_FOUND;
  }

  *Interface = mProto;
  mProto     = NULL;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI FakeCloseEvent (EFI_EVENT Event) { mNotifyFn = NULL; return EFI_SUCCESS; }

static EFI_BOOT_SERVICES  mBs;
EFI_BOOT_SERVICES         *gBS = &mBs;

// ---- Helpers -----------------------------------------------------------------

static void *
ReadFile (const char *Path, long *Size)
{
  FILE  *F = fopen (Path, "rb");
  void  *Buf;

  if (F == NULL) {
    return NULL;
  }

  fseek (F, 0, SEEK_END);
  *Size = ftell (F);
  fseek (F, 0, SEEK_SET);
  Buf = malloc (*Size);
  if (fread (Buf, 1, *Size, F) != (size_t)*Size) {
    exit (2);
  }

  fclose (F);
  return Buf;
}

// The DTB with /chosen/bootargs set, at Where (or a new buffer).
static void *
WithBootargs (const void *Dtb, const char *Args, void *Where)
{
  int   Size = fdt_totalsize (Dtb) + 4096;
  void  *Out = Where ? Where : malloc (Size);
  int   Chosen;

  CHECK_EQ (fdt_open_into (Dtb, Out, Size), 0);
  Chosen = fdt_path_offset (Out, "/chosen");
  if (Chosen < 0) {
    Chosen = fdt_add_subnode (Out, 0, "chosen");
  }

  if (Args != NULL) {
    CHECK_EQ (fdt_setprop_string (Out, Chosen, "bootargs", Args), 0);
  }

  fdt_pack (Out);
  return Out;
}

static int
FirstWriteTo (UINTN Addr)
{
  int  i;

  for (i = 0; i < fake_trace_count; i++) {
    if (fake_trace[i].Addr == Addr) {
      return i;
    }
  }

  return -1;
}

static int
VopDsiWrites (void)
{
  int  i, n = 0;

  for (i = 0; i < fake_trace_count; i++) {
    if ((fake_trace[i].Addr >> 16) == 0xFF46 || (fake_trace[i].Addr >> 16) == 0xFF45 || (fake_trace[i].Addr >> 16) == 0xFF2E) {
      n++;
    }
  }

  return n;
}

// A fresh driver and machine: U-Boot's screen on (Fb != NULL) or dark.
static void
Boot (const void *BootFdt, const RK_FRAMEBUFFER_INFO *Fb, unsigned Mode)
{
  fake_reset ();
  fake_pmic_count = 0;
  fake_set (0xFF2BC000, (1 << 12) | 99);
  fake_set (0xFF2BC004, (1 << 12) | (1 << 6) | 2);
  fake_set (0xFF2BC020, 1);
  fake_set (0xFF2B0100 + 52 * 4, (11 << 8) | 11);

  mDisplay = NULL; mInfo = NULL; mBootloaderFdt = 0; mMode = 0; mGopInstalled = FALSE; mDone = FALSE;
  mNotifyEvent = NULL; mNotifyRegistration = NULL;
  mGopCalls = 0; mPagesCalls = 0; mNotifyFn = NULL; mProto = NULL;
  memset (&mGop, 0, sizeof (mGop));
  mLogFbBase = 0;

  memset (&mHob, 0, sizeof (mHob));
  mHaveHob                    = TRUE;
  mHob.Info.Version           = 2;
  mHob.Info.BootloaderFdt     = (UINTN)BootFdt;
  mHob.Info.BootloaderFdtSize = BootFdt ? fdt_totalsize (BootFdt) : 0;
  if (Fb != NULL) {
    mHob.Info.FramebufferValid = 1;
    mHob.Info.Framebuffer      = *Fb;
  }

  mock_pcd_mode = (unsigned char)Mode;
  CHECK_EQ (RkDisplayDxeEntryPoint (NULL, NULL), EFI_SUCCESS);
}

int
main (int argc, char **argv)
{
  long                 Size, ScreenSize;
  void                 *Android, *Screen, *Boot1, *Window;
  RK_FRAMEBUFFER_INFO  Fb;
  void                 *FbMem;

  if (argc != 3) {
    printf ("usage: test_entry <android.dtb> <screen.dtb>\n");
    return 2;
  }

  Android = ReadFile (argv[1], &Size);
  Screen  = ReadFile (argv[2], &ScreenSize);
  CHECK (Android != NULL && Screen != NULL);
  fake_verbose = getenv ("VERBOSE") != NULL;

  mBs.AllocatePages          = FakeAllocatePages;
  mBs.CreateEvent            = FakeCreateEvent;
  mBs.RegisterProtocolNotify = FakeRegisterProtocolNotify;
  mBs.SignalEvent            = FakeSignalEvent;
  mBs.LocateProtocol         = FakeLocateProtocol;
  mBs.CloseEvent             = FakeCloseEvent;

  // U-Boot's 640x480 32 bpp screen
  FbMem = mmap (NULL, 640 * 480 * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  memset (&Fb, 0, sizeof (Fb));
  Fb.Base = (UINTN)FbMem; Fb.Width = 640; Fb.Height = 480; Fb.Stride = 640 * 4; Fb.BitsPerPixel = 32; Fb.VopBase = 0xFF460000;

  // 1. Adopt (PCD default): GOP on U-Boot's framebuffer, not one register written.
  Boot1 = WithBootargs (Android, "andr36oid.hwrev=v1", NULL);
  Boot (Boot1, &Fb, 0);
  CHECK_EQ (mGopCalls, 1);
  CHECK_EQ (mGop.Base, Fb.Base);
  CHECK_EQ (fake_trace_count, 0);
  CHECK_EQ (mPagesCalls, 0);
  CHECK (mDone);
  printf ("adopt: GOP on U-Boot's framebuffer, %d register writes\n", fake_trace_count);

  // 2. andr36oid.display=reinit: shut down and up again, same framebuffer.
  Boot1 = WithBootargs (Android, "foo andr36oid.display=reinit bar", NULL);
  Boot (Boot1, &Fb, 0);
  CHECK_EQ (mMode, MODE_REINIT);
  CHECK_EQ (mGopCalls, 1);
  CHECK_EQ (mGop.Base, Fb.Base);
  CHECK_EQ (mPagesCalls, 0);
  CHECK (VopDsiWrites () > 100);
  CHECK_EQ (fake_get (0xFF4600A0), (UINT32)Fb.Base);
  CHECK_EQ (fake_get (0xFF460018) & BIT1, 0);
  CHECK (FirstWriteTo (0xFF2B030C) >= 0);
  printf ("reinit: %d writes, framebuffer kept, %d errors\n", fake_trace_count, fake_errors);

  // 3. The PCD asks for reinit, bootargs override back to adopt.
  Boot1 = WithBootargs (Android, "andr36oid.display=adopt", NULL);
  Boot (Boot1, &Fb, 1);
  CHECK_EQ (mMode, MODE_ADOPT);
  CHECK_EQ (fake_trace_count, 0);

  // 4. Reinit of a 16 bpp U-Boot screen keeps it at 16 bpp (RGB565, rb swap).
  {
    RK_FRAMEBUFFER_INFO  Fb16 = Fb;

    Fb16.BitsPerPixel = 16;
    Fb16.Stride       = 640 * 2;
    Boot (WithBootargs (Android, "andr36oid.display=reinit", NULL), &Fb16, 0);
    CHECK_EQ ((fake_get (0xFF460090) >> 4) & 7, 2);
    CHECK_EQ ((fake_get (0xFF460090) >> 12) & 1, 1);
    CHECK_EQ (fake_get (0xFF460098) & 0x1FFF, 320);
    CHECK_EQ (mGop.BitsPerPixel, 16);
  }

  // 5. Reinit with a framebuffer of another size: new 32 bpp framebuffer.
  {
    RK_FRAMEBUFFER_INFO  Small = Fb;

    Small.Width = 320; Small.Height = 240; Small.Stride = 320 * 4;
    Boot (WithBootargs (Android, "andr36oid.display=reinit", NULL), &Small, 0);
    CHECK_EQ (mPagesCalls, 1);
    CHECK (mGop.Base != Small.Base);
    CHECK_EQ (mGop.Width, 640);
    CHECK_EQ (mGop.BitsPerPixel, 32);
    CHECK_EQ (*(UINT32 *)(UINTN)mGop.Base, 0);   // cleared to black
    CHECK_EQ (fake_get (0xFF4600A0), (UINT32)mGop.Base);
  }

  // 6. Dark screen: brought up whatever the mode, new framebuffer.
  Boot (WithBootargs (Android, NULL, NULL), NULL, 0);
  CHECK_EQ (mPagesCalls, 1);
  CHECK_EQ (mGopCalls, 1);
  CHECK_EQ (fake_get (0xFF4600A0), (UINT32)mGop.Base);
  CHECK (mDone);
  printf ("dark screen: brought up, GOP on a new %ux%u framebuffer\n", mGop.Width, mGop.Height);

  // 7. Dark screen, no usable FDT: waits for RK_SCREEN_FDT_PROTOCOL.
  {
    RK_SCREEN_FDT_PROTOCOL  Proto = { RK_SCREEN_FDT_PROTOCOL_REVISION, RkScreenFdtFromBootPartition, Screen, ScreenSize };

    Boot (NULL, NULL, 0);
    CHECK (!mDone);
    CHECK_EQ (mGopCalls, 0);
    CHECK (mNotifyFn != NULL);
    mProto = &Proto;
    FakeSignalEvent (mNotifyEvent);
    CHECK (mDone);
    CHECK_EQ (mGopCalls, 1);
    CHECK (VopDsiWrites () > 100);
    printf ("protocol notify: brought up from the protocol's FDT\n");
  }

  // 8. Screen on, reinit, no FDT yet: adopted GOP now, reinit on the same
  //    framebuffer when the protocol arrives.
  {
    RK_SCREEN_FDT_PROTOCOL  Proto = { RK_SCREEN_FDT_PROTOCOL_REVISION, RkScreenFdtFromBootPartition, Screen, ScreenSize };

    Boot (NULL, &Fb, 1);
    CHECK_EQ (mGopCalls, 1);
    CHECK_EQ (fake_trace_count, 0);
    mProto = &Proto;
    FakeSignalEvent (mNotifyEvent);
    CHECK (mDone);
    CHECK_EQ (mGopCalls, 1);
    CHECK_EQ (fake_get (0xFF4600A0), (UINT32)Fb.Base);
  }

  // 9. andr36oid.screen_fdt: the screen DTB in the reserved window wins over
  //    the Android DTB; outside the window it is refused.
  Window = mmap ((void *)RK_BOOTLOADER_FDT_BASE, RK_BOOTLOADER_FDT_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (Window == (void *)RK_BOOTLOADER_FDT_BASE) {
    char  Args[128];

    snprintf (Args, sizeof (Args), "andr36oid.screen_fdt=0x%x andr36oid.display=reinit", RK_BOOTLOADER_FDT_BASE + 0x80000);
    WithBootargs (Android, Args, Window);
    memcpy ((char *)Window + 0x80000, Screen, ScreenSize);
    Boot (Window, &Fb, 0);
    CHECK (mDone);
    CHECK (mDisplay->Panel.Fdt == (char *)Window + 0x80000);
    CHECK_EQ (fake_errors, 0);
    printf ("screen_fdt in the window: used (%d writes)\n", fake_trace_count);

    WithBootargs (Android, "andr36oid.screen_fdt=0x3000000", Window);
    Boot (Window, &Fb, 0);
    CHECK (fake_errors >= 1);
    CHECK (mDone);          // falls back to the bootloader FDT (adopt)

    WithBootargs (Android, "andr36oid.screen_fdt=0x1f90000", Window);
    Boot (Window, &Fb, 0);
    CHECK (fake_errors >= 1);
    munmap (Window, RK_BOOTLOADER_FDT_SIZE);
  } else {
    printf ("(0x%x not mappable here, screen_fdt case skipped)\n", RK_BOOTLOADER_FDT_BASE);
  }

  // 10. Bootargs parsing edge cases
  {
    UINTN        Len;
    CONST CHAR8  *A = "xandr36oid.display=reinit andr36oid.display andr36oid.display=adopt";

    CHECK (BootArg (A, AsciiStrLen (A), "andr36oid.display", &Len) == strstr (A, "=adopt") + 1);
    CHECK_EQ (Len, 5);
    CHECK (BootArg ("andr36oid.displayx=1", 20, "andr36oid.display", &Len) == NULL);
    CHECK (BootArg ("", 0, "andr36oid.display", &Len) == NULL);
  }

  DONE ();
}

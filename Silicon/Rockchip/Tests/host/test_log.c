// RkSerialPortLib: the RAM log ring, the previous-boot log, the on-screen log
// and UART2, against the contract in RkLogLib.h (newest bytes kept, previous
// log only for starts that died in the firmware, screen log on 32/16 bpp only,
// "anything else turns the screen log off"), the header layouts of earlier
// commits (9443304, fc551f2, 40fe5ec: 64 bytes) and U-Boot's ns16550 putc
// (wait for LSR.THRE, write THR). The log lives at its real address with
// no-access pages around it; every framebuffer has a no-access page after it
// and canary bytes in its line padding.
//
// What the screen shows is compared with a plain terminal model (wrap at the
// last column, clear and start at the top when the last row is full) using
// glyphs the library draws on a one-cell screen, after checking those glyphs
// look like the characters ('|' one column, '-' one row, '_' below '-', ...).
//
// SPDX-License-Identifier: BSD-2-Clause-Patent

#define _GNU_SOURCE
#include <sys/mman.h>
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/RkLogLib.h>
#include <Library/RkPlatformInfoLib.h>
#include <Library/SerialPortLib.h>
#include <Soc/Rk3326.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "known.h"

static BOOLEAN  mock_screen = TRUE, mock_uart = FALSE;

#define _PCD_GET_MODE_BOOL_PcdRkLogScreen  mock_screen
#define _PCD_VALUE_PcdRkLogUart            mock_uart

// ---- what U-Boot left on the VOP -------------------------------------------------

static BOOLEAN              mFbOn;
static RK_FRAMEBUFFER_INFO  mFb;

EFI_STATUS EFIAPI RkGetActiveFramebuffer (OUT RK_FRAMEBUFFER_INFO *Info)
{
  if (!mFbOn) {
    return EFI_NOT_FOUND;
  }

  *Info = mFb;
  return EFI_SUCCESS;
}

// ---- UART2: a 16550 whose THRE comes back some polls after each byte ---------------

#define UART_LSR_ADDR  (RK3326_UART2_BASE + 0x14)
#define UART_THR_ADDR  (RK3326_UART2_BASE + 0x00)

static BOOLEAN        uart_never;         // THRE never sets (no clock, pins muxed away)
static UINT32         uart_busy;          // polls THRE stays clear after a byte
static UINT32         uart_busy_left;
static unsigned long  uart_polls;
static char           uart_out[1 << 16];
static unsigned long  uart_len;

UINT32 EFIAPI MmioRead32 (UINTN a)
{
  if (a == UART_LSR_ADDR) {
    uart_polls++;
    if (uart_never) {
      return 0;
    }

    if (uart_busy_left > 0) {
      uart_busy_left--;
      return 0x01;          // data ready bit only: transmitter busy
    }

    return 0x60;            // THRE | TEMT
  }

  printf ("FAIL: log library read 0x%lx\n", (unsigned long)a);
  exit (1);
}

UINT32 EFIAPI MmioWrite32 (UINTN a, UINT32 v)
{
  if (a == UART_THR_ADDR) {
    if (uart_never || (uart_busy_left > 0)) {
      printf ("FAIL: byte 0x%02x written to THR while the transmitter was busy\n", v);
      g_fails++;
    }

    if (uart_len < sizeof (uart_out)) {
      uart_out[uart_len] = (char)v;
    }

    uart_len++;
    uart_busy_left = uart_busy;
    return v;
  }

  printf ("FAIL: log library wrote 0x%x to 0x%lx\n", v, (unsigned long)a);
  exit (1);
}

VOID * EFIAPI CopyMem (OUT VOID *D, IN CONST VOID *S, IN UINTN L) { memmove (D, S, L); return D; }

#include "RkSerialPortLib.c"

// ---- helpers ------------------------------------------------------------------------

static void Put (CONST char *S)
{
  UINTN  N = strlen (S);

  CHECK_EQ (SerialPortWrite ((UINT8 *)S, N), N);
}

static char  rd[RK_LOG_SIZE];

// The whole log as RkLogRead returns it, NUL-terminated (it holds no NULs here).
static UINTN ReadAll (BOOLEAN Previous)
{
  UINTN  N = RkLogRead (Previous, rd, sizeof (rd) - 1);

  CHECK_EQ (N, RkLogSize (Previous));
  rd[N] = 0;
  return N;
}

static void NewBoot (void)
{
  RkLogStart ();
  CHECK (HeaderValid (CURRENT));
  CHECK_EQ (RkLogSize (FALSE), 0);
}

static void WipeLog (void) { memset ((void *)(UINTN)RK_LOG_BASE, 0, RK_LOG_SIZE); }

static UINT8 Pat (UINT64 i) { return (UINT8)('!' + (i * 7 + (i >> 9)) % 90); }   // printable, aperiodic enough

// ---- the ring ---------------------------------------------------------------------------

static void TestRing (void)
{
  static UINT8  Big[2 * 0x80000];
  UINT64        DataSize, i;
  UINTN         N, Sizes[] = { 1, 2, 7, 4095, 4096, 65537 };
  UINTN         k;

  mFbOn = FALSE;
  WipeLog ();
  NewBoot ();
  CHECK_EQ (RkLogSize (TRUE), 0);
  DataSize = CURRENT->DataSize;
  CHECK (DataSize >= 0x70000);        // most of the 512 KB half

  Put ("hello\n");
  CHECK_EQ (ReadAll (FALSE), 6);
  CHECK (strcmp (rd, "hello\n") == 0);

  // Nothing to read for a NULL buffer or a zero-size one.
  CHECK_EQ (RkLogRead (FALSE, NULL, 100), 0);
  CHECK_EQ (RkLogRead (FALSE, rd, 0), 0);
  CHECK_EQ (SerialPortWrite (NULL, 5), 0);
  CHECK_EQ (SerialPortWrite ((UINT8 *)"x", 0), 0);
  CHECK_EQ (RkLogSize (FALSE), 6);

  // Past the end of the ring: the newest DataSize bytes, oldest first.
  NewBoot ();
  for (i = 0; i < DataSize + 1000; i++) {
    Big[i] = Pat (i);
  }

  CHECK_EQ (SerialPortWrite (Big, DataSize + 1000), DataSize + 1000);
  CHECK_EQ (RkLogSize (FALSE), DataSize);
  N = RkLogRead (FALSE, rd, sizeof (rd));
  CHECK_EQ (N, DataSize);
  CHECK (memcmp (rd, Big + 1000, DataSize) == 0);

  // A short buffer gets the newest bytes.
  for (k = 0; k < ARRAY_SIZE (Sizes); k++) {
    memset (rd, 0, Sizes[k] + 1);
    N = RkLogRead (FALSE, rd, Sizes[k]);
    CHECK_EQ (N, Sizes[k]);
    CHECK (memcmp (rd, Big + DataSize + 1000 - Sizes[k], Sizes[k]) == 0);
  }

  // Exactly one ring's worth, then one byte more.
  NewBoot ();
  CHECK_EQ (SerialPortWrite (Big, DataSize), DataSize);
  N = RkLogRead (FALSE, rd, sizeof (rd));
  CHECK (N == DataSize && memcmp (rd, Big, DataSize) == 0);
  CHECK_EQ (SerialPortWrite (Big + DataSize, 1), 1);
  N = RkLogRead (FALSE, rd, sizeof (rd));
  CHECK (N == DataSize && memcmp (rd, Big + 1, DataSize) == 0);

  // A firmware that has run long enough to write more than 4 GB of log
  // (a UINT32 count would wrap): the ring position keeps following the count.
  NewBoot ();
  CURRENT->Written = (1ULL << 32) - 300;
  CHECK_EQ (SerialPortWrite (Big, 1000), 1000);
  CHECK_EQ (CURRENT->Written, (1ULL << 32) + 700);
  N = RkLogRead (FALSE, rd, 1000);
  CHECK (N == 1000 && memcmp (rd, Big, 1000) == 0);
  CURRENT->Written = 0xFFFFFFFFFFFFF000ULL;       // and near the end of a UINT64
  CHECK_EQ (SerialPortWrite (Big + 5000, 2000), 2000);
  N = RkLogRead (FALSE, rd, 2000);
  CHECK (N == 2000 && memcmp (rd, Big + 5000, 2000) == 0);
}

// ---- the previous boot's log ---------------------------------------------------------

// The 64-byte header of 9443304/fc551f2/40fe5ec, as it may still sit in RAM
// after an update.
typedef struct {
  UINT32  Magic, HeaderSize;
  UINT64  Written;
  UINT32  DataSize, FbValid;
  UINT64  FbBase;
  UINT32  FbWidth, FbHeight, FbStride, FbBytesPerPixel, Column, Row, ScreenOff, HandedOff;
} OLD_HEADER;

static void TestPrevious (void)
{
  OLD_HEADER  *Old = (OLD_HEADER *)(UINTN)RK_LOG_BASE;
  UINTN       N;

  _Static_assert (sizeof (OLD_HEADER) == 64, "old header");
  mFbOn = FALSE;

  // A start that died in the firmware: its log is the previous one next time.
  WipeLog ();
  NewBoot ();
  Put ("boot one\n");
  NewBoot ();
  CHECK_EQ (ReadAll (TRUE), 9);
  CHECK (strcmp (rd, "boot one\n") == 0);
  CHECK_EQ (ReadAll (FALSE), 0);

  // Only the last one is kept.
  Put ("boot two\n");
  NewBoot ();
  ReadAll (TRUE);
  CHECK (strcmp (rd, "boot two\n") == 0);

  // Handed to Linux: Linux reused the RAM, nothing is kept.
  Put ("boot three\n");
  RkLogHandoff (TRUE);
  NewBoot ();
  CHECK_EQ (RkLogSize (TRUE), 0);
  CHECK_EQ (RkLogRead (TRUE, rd, sizeof (rd)), 0);

  // Handoff taken back (the kernel did not start): kept.
  Put ("boot four\n");
  RkLogHandoff (TRUE);
  RkLogHandoff (FALSE);
  NewBoot ();
  ReadAll (TRUE);
  CHECK (strcmp (rd, "boot four\n") == 0);

  // A previous log that wrapped: its newest bytes.
  {
    static UINT8  Big[0x80000 + 5000];
    UINT64        DataSize = CURRENT->DataSize, i;

    for (i = 0; i < DataSize + 5000; i++) {
      Big[i] = Pat (i * 3);
    }

    SerialPortWrite (Big, DataSize + 5000);
    NewBoot ();
    N = RkLogRead (TRUE, rd, sizeof (rd));
    CHECK (N == DataSize && memcmp (rd, Big + 5000, DataSize) == 0);
    N = RkLogRead (TRUE, rd, 10);
    CHECK (N == 10 && memcmp (rd, Big + DataSize + 4990, 10) == 0);
  }

  // Garbage where the header should be (cold RAM, or a kernel that used it):
  // no previous log, and a fresh current one.
  {
    static CONST struct { CONST char *Name; UINTN Off; UINT32 Val; } Bad[] = {
      { "magic", 0, 0x474C4B53 }, { "header size", 4, 71 }, { "data size", 16, 0x7FFB7 }, { "data size 0", 16, 0 },
    };
    UINTN  b;

    for (b = 0; b < ARRAY_SIZE (Bad); b++) {
      NewBoot ();
      Put ("damaged\n");
      memcpy ((UINT8 *)(UINTN)RK_LOG_BASE + Bad[b].Off, &Bad[b].Val, 4);
      NewBoot ();
      if (RkLogSize (TRUE) != 0) {
        printf ("FAIL: previous log kept with a bad %s\n", Bad[b].Name);
      }

      CHECK_EQ (RkLogSize (TRUE), 0);
    }
  }

  // A count far beyond the ring (RAM bit flips): still the ring's bytes, no
  // read outside the log.
  NewBoot ();
  Put ("x");
  CURRENT->Written = 0x8000000000000123ULL;
  NewBoot ();
  CHECK_EQ (RkLogSize (TRUE), CURRENT->DataSize);
  CHECK_EQ (RkLogRead (TRUE, rd, sizeof (rd)), CURRENT->DataSize);

  // The 64-byte header of an older firmware: either read right or not at all.
  WipeLog ();
  memset (Old, 0, sizeof (*Old));
  Old->Magic      = SIGNATURE_32 ('R', 'K', 'L', 'G');
  Old->HeaderSize = sizeof (OLD_HEADER);
  Old->DataSize   = RK_LOG_SIZE / 2 - sizeof (OLD_HEADER);
  Old->Written    = 15;
  memcpy ((UINT8 *)Old + sizeof (OLD_HEADER), "old firmware!\r\n", 15);
  NewBoot ();
  N = ReadAll (TRUE);
  CHECK ((N == 0) || ((N == 15) && (memcmp (rd, "old firmware!\r\n", 15) == 0)));
  Put ("new\n");
  ReadAll (FALSE);
  CHECK (strcmp (rd, "new\n") == 0);

  // Every module's DebugLib calls SerialPortInitialize: only the first one
  // sets the log up, the others keep writing to it.
  WipeLog ();
  CHECK_EQ (SerialPortInitialize (), RETURN_SUCCESS);
  Put ("first ");
  CHECK_EQ (SerialPortInitialize (), RETURN_SUCCESS);
  Put ("second\n");
  ReadAll (FALSE);
  CHECK (strcmp (rd, "first second\n") == 0);

  // Before RkLogStart / SerialPortInitialize nothing is written anywhere.
  WipeLog ();
  CHECK_EQ (SerialPortWrite ((UINT8 *)"lost", 4), 0);
  CHECK_EQ (RkLogSize (FALSE), 0);
  RkLogScreen (FALSE);
  RkLogHandoff (TRUE);
  RkLogSetFramebuffer (0x1000, 640, 480, 2560, 32);
  {
    UINT8  Zero[64] = { 0 };
    CHECK (memcmp ((void *)(UINTN)RK_LOG_BASE, Zero, sizeof (Zero)) == 0);
  }
}

// ---- UART2 ---------------------------------------------------------------------------

static void TestUart (void)
{
  static char  Text[3000];
  UINTN        i;

  mFbOn = FALSE;
  mock_uart = TRUE;
  for (i = 0; i < sizeof (Text); i++) {
    Text[i] = (char)Pat (i);
  }

  // A working UART: every byte, in order, each after THRE.
  WipeLog ();
  NewBoot ();
  uart_never = FALSE; uart_busy = 3; uart_busy_left = 0; uart_len = 0; uart_polls = 0;
  SerialPortWrite ((UINT8 *)Text, sizeof (Text));
  CHECK_EQ (uart_len, sizeof (Text));
  CHECK (memcmp (uart_out, Text, sizeof (Text)) == 0);
  CHECK_EQ (CURRENT->UartDead, 0);

  // A slow one (THRE back after 50000 polls, under the give-up limit): no byte lost.
  uart_busy = 50000; uart_len = 0;
  SerialPortWrite ((UINT8 *)"slow", 4);
  CHECK_EQ (uart_len, 4);
  CHECK (memcmp (uart_out, "slow", 4) == 0);
  CHECK_EQ (CURRENT->UartDead, 0);

  // A dead one (no clock, pins muxed away): give up once, then stop waiting;
  // the log and the screen still get everything.
  uart_never = TRUE; uart_polls = 0; uart_len = 0;
  SerialPortWrite ((UINT8 *)Text, sizeof (Text));
  CHECK_EQ (uart_len, 0);
  CHECK (uart_polls <= 200000);            // a few ms in total, not per byte
  printf ("dead UART: %lu polls for %lu bytes\n", uart_polls, (unsigned long)sizeof (Text));
  CHECK_EQ (CURRENT->UartDead, 1);
  CHECK (RkLogRead (FALSE, rd, sizeof (Text)) == sizeof (Text) && memcmp (rd, Text, sizeof (Text)) == 0);

  // Later modules in the same start do not wait again (the flag is in RAM).
  uart_polls = 0;
  SerialPortWrite ((UINT8 *)"more", 4);
  CHECK_EQ (uart_polls, 0);

  // The next start tries the UART again.
  uart_never = FALSE; uart_busy = 0; uart_len = 0;
  NewBoot ();
  CHECK_EQ (CURRENT->UartDead, 0);
  Put ("back\n");
  CHECK_EQ (uart_len, 5);

  // UART off at build time: no register touched.
  mock_uart = FALSE;
  uart_polls = 0; uart_len = 0;
  Put ("quiet\n");
  CHECK_EQ (uart_polls, 0);
  CHECK_EQ (uart_len, 0);
}

// ---- framebuffers ---------------------------------------------------------------------

#define CANARY  0xA5
#define OLDPIX  0x33

typedef struct {
  UINT8   *Map, *Body, *Base;
  size_t  MapLen, BodyLen;
  UINT32  W, H, Stride, Bpp;
} FB;

static FB FbNew (UINT32 W, UINT32 H, UINT32 Stride, UINT32 Bpp)
{
  FB      F = { 0 };
  size_t  Len = (size_t)Stride * H, y;

  F.BodyLen = (Len + 4095) & ~(size_t)4095;
  F.MapLen  = F.BodyLen + 2 * 4096;
  F.Map     = mmap (NULL, F.MapLen, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if ((F.Map == MAP_FAILED) || (mprotect (F.Map + 4096, F.BodyLen, PROT_READ | PROT_WRITE) != 0)) {
    printf ("FAIL: cannot map a framebuffer\n");
    exit (1);
  }

  F.Body = F.Map + 4096;
  F.Base = F.Body + ((F.BodyLen - Len) & ~(size_t)63);    // last line ends near the guard page
  F.W = W; F.H = H; F.Stride = Stride; F.Bpp = Bpp;
  memset (F.Body, CANARY, F.BodyLen);
  for (y = 0; y < H; y++) {
    memset (F.Base + y * Stride, OLDPIX, (size_t)W * (Bpp / 8));
  }

  return F;
}

static void FbFree (FB *F) { munmap (F->Map, F->MapLen); }

// Nothing outside the visible pixels changed (line padding, before and after).
static BOOLEAN FbOutsideIntact (CONST FB *F)
{
  size_t  i;

  for (i = 0; i < F->BodyLen; i++) {
    UINT8   *P = F->Body + i;
    BOOLEAN In = FALSE;

    if ((P >= F->Base) && (P < F->Base + (size_t)F->Stride * F->H)) {
      In = ((size_t)(P - F->Base) % F->Stride) < (size_t)F->W * (F->Bpp / 8);
    }

    if (!In && (*P != CANARY)) {
      printf ("    byte at +0x%lx (outside the visible pixels) changed to 0x%02x\n", (long)(P - F->Base), *P);
      return FALSE;
    }
  }

  return TRUE;
}

enum { PX_OLD, PX_ON, PX_OFF, PX_OTHER };

static int Px (CONST FB *F, UINT32 x, UINT32 y)
{
  UINT8  *P = F->Base + (size_t)y * F->Stride + (size_t)x * (F->Bpp / 8);

  if (F->Bpp == 32) {
    UINT32  V = *(UINT32 *)P;
    return V == 0x33333333 ? PX_OLD : V == 0xFFFFFFFF ? PX_ON : (V == 0xFF000000 || V == 0) ? PX_OFF : PX_OTHER;
  }

  {
    UINT16  V = *(UINT16 *)P;
    return V == 0x3333 ? PX_OLD : V == 0xFFFF ? PX_ON : V == 0 ? PX_OFF : PX_OTHER;
  }
}

typedef struct { UINT8 Row[GLYPH_H]; } GLY;     // bit x of Row[y]: pixel on

static GLY  Ref[128];

// The cell's lit pixels; *Old: every pixel still the old picture.
static GLY Cell (CONST FB *F, UINT32 r, UINT32 c, BOOLEAN *Old, BOOLEAN *Bad)
{
  GLY     G;
  UINT32  x, y;

  memset (&G, 0, sizeof (G));
  *Old = TRUE;
  *Bad = FALSE;
  for (y = 0; y < GLYPH_H; y++) {
    for (x = 0; x < GLYPH_W; x++) {
      int  P = Px (F, c * GLYPH_W + x, r * GLYPH_H + y);

      if (P != PX_OLD) {
        *Old = FALSE;
      }

      if (P == PX_ON) {
        G.Row[y] |= (UINT8)(1u << x);
      } else if (P == PX_OTHER) {
        *Bad = TRUE;
      }
    }
  }

  return G;
}

// The terminal model.
typedef struct {
  UINT32   Cols, Rows, Row, Col;
  BOOLEAN  Cleared;
  char     *Grid;       // ' ' blank, 0 never touched since the framebuffer was set
} TERM;

static void TermInit (TERM *T, UINT32 Cols, UINT32 Rows)
{
  free (T->Grid);
  memset (T, 0, sizeof (*T));
  T->Cols = Cols;
  T->Rows = Rows;
  T->Grid = calloc ((size_t)Cols * Rows + 1, 1);
}

static void TermPut (TERM *T, char C)
{
  if (C == '\n') {
    T->Col = 0;
    T->Row++;
  } else if (C == '\r') {
    T->Col = 0;
    return;
  } else if (((UINT8)C < 32) || ((UINT8)C > 126)) {
    return;
  }

  if (T->Row >= T->Rows) {
    memset (T->Grid, ' ', (size_t)T->Cols * T->Rows);
    T->Cleared = TRUE;
    T->Row = T->Col = 0;
  }

  if (C == '\n') {
    return;
  }

  T->Grid[T->Row * T->Cols + T->Col] = C;
  if (++T->Col >= T->Cols) {
    T->Col = 0;
    T->Row++;
  }
}

// The screen shows what the model says.
static BOOLEAN ScreenIs (CONST FB *F, CONST TERM *T, BOOLEAN Quiet)
{
  UINT32  r, c, x, y;

  for (r = 0; r < T->Rows; r++) {
    for (c = 0; c < T->Cols; c++) {
      char     Ch = T->Grid[r * T->Cols + c];
      BOOLEAN  Old, Bad;
      GLY      G = Cell (F, r, c, &Old, &Bad);
      GLY      Want;

      memset (&Want, 0, sizeof (Want));
      if ((Ch != 0) && (Ch != ' ')) {
        Want = Ref[(UINT8)Ch];
      }

      if (Bad || ((Ch == 0) && !Old) || ((Ch != 0) && (Old || (memcmp (&G, &Want, sizeof (G)) != 0)))) {
        if (!Quiet) {
          printf ("    cell row %u col %u: want '%c'%s, got %s%s\n", r, c, Ch ? Ch : '?', Ch ? "" : " (untouched)",
                  Old ? "the old picture" : "something else", Bad ? " with odd pixel values" : "");
        }

        return FALSE;
      }
    }
  }

  // Right and bottom margins: never glyph pixels (a clear may zero them).
  for (y = 0; y < F->H; y++) {
    for (x = (y < T->Rows * GLYPH_H) ? T->Cols * GLYPH_W : 0; x < F->W; x++) {
      int  P = Px (F, x, y);

      if ((P == PX_ON) || (P == PX_OTHER) || (!T->Cleared && (P != PX_OLD))) {
        if (!Quiet) {
          printf ("    margin pixel %u,%u changed\n", x, y);
        }

        return FALSE;
      }
    }
  }

  return TRUE;
}

static void SetFb (CONST FB *F)
{
  RkLogSetFramebuffer ((UINT64)(UINTN)F->Base, F->W, F->H, F->Stride, F->Bpp);
}

// Every printable character on a one-cell screen gives the reference glyph.
static void TestFont (void)
{
  FB      F = FbNew (GLYPH_W, GLYPH_H, GLYPH_W * 4, 32);
  int     ch, Lit;
  UINT32  x, y;
  BOOLEAN Old, Bad;

  mFbOn = FALSE;
  WipeLog ();
  NewBoot ();
  SetFb (&F);
  CHECK (CURRENT->FbValid);
  for (ch = 32; ch <= 126; ch++) {
    char  S[2] = { (char)ch, 0 };

    Put (S);
    Ref[ch] = Cell (&F, 0, 0, &Old, &Bad);
    CHECK (!Old && !Bad);
  }

  CHECK (FbOutsideIntact (&F));

  // Plausible glyphs: space blank, everything else lit, all different, the
  // sixth column is the gap between characters.
  for (ch = 33; ch <= 126; ch++) {
    int  o;

    for (Lit = 0, y = 0; y < GLYPH_H; y++) {
      Lit |= Ref[ch].Row[y];
      CHECK ((Ref[ch].Row[y] & (1u << (GLYPH_W - 1))) == 0);
    }

    CHECK (Lit != 0);
    for (o = ch + 1; o <= 126; o++) {
      if (memcmp (&Ref[ch], &Ref[o], sizeof (GLY)) == 0) {
        printf ("FAIL: '%c' and '%c' look the same\n", ch, o);
        g_fails++;
      }
    }
  }

  for (y = 0; y < GLYPH_H; y++) {
    CHECK_EQ (Ref[' '].Row[y], 0);
  }

  // Shapes: '|' is one column over several rows, '-' and '_' one row each
  // with '_' lower, '.' in the lower half, '\'' and '`' in the upper half,
  // 'L' has its bar at the bottom and its stem on the left.
  {
    int  RowsOn = 0, Cols = 0, DashY = -1, UndY = -1, LowY = 0, HighY = GLYPH_H, x0 = -1;

    for (y = 0; y < GLYPH_H; y++) {
      if (Ref['|'].Row[y]) {
        RowsOn++;
        Cols |= Ref['|'].Row[y];
      }

      if (Ref['-'].Row[y]) {
        CHECK (DashY < 0);
        DashY = (int)y;
      }

      if (Ref['_'].Row[y]) {
        CHECK (UndY < 0);
        UndY = (int)y;
      }

      if (Ref['.'].Row[y] && ((int)y < HighY)) {
        HighY = (int)y;
      }

      if (Ref['\''].Row[y] || Ref['`'].Row[y]) {
        LowY = (int)y;
      }
    }

    CHECK (RowsOn >= 6);
    CHECK ((Cols & (Cols - 1)) == 0);
    CHECK ((DashY >= 0) && (UndY > DashY));
    CHECK (HighY >= GLYPH_H / 2);
    CHECK (LowY < GLYPH_H / 2);
    for (x = 0; x < GLYPH_W; x++) {
      if (Ref['L'].Row[2] & (1u << x)) {
        x0 = (int)x;
        break;
      }
    }

    CHECK (x0 >= 0);
    for (y = 0, Lit = 0; y < GLYPH_H; y++) {
      Lit = Ref['L'].Row[y] ? (int)y : Lit;
    }

    CHECK (__builtin_popcount (Ref['L'].Row[Lit]) >= 3);
    CHECK ((Ref['L'].Row[Lit] & (1u << x0)) != 0);
  }

  FbFree (&F);
}

static TERM  Tm;

// Write S, keep the model in step, compare.
static void Say (CONST FB *F, CONST char *S, CONST char *What)
{
  CONST char  *P;

  Put (S);
  for (P = S; *P; P++) {
    TermPut (&Tm, *P);
  }

  if (!ScreenIs (F, &Tm, FALSE)) {
    printf ("FAIL: screen after '%s' (%ux%u stride %u, %u bpp)\n", What, F->W, F->H, F->Stride, F->Bpp);
    g_fails++;
  }

  CHECK (FbOutsideIntact (F));
}

static void TestScreen (void)
{
  static CONST struct { UINT32 W, H, Stride, Bpp; } Shapes[] = {
    { 640, 480, 2560, 32 },     // R36S panel, our own 32 bpp buffer
    { 640, 480, 1280, 16 },
    { 480, 320, 1984, 32 },     // stride wider than a line
    { 480, 320, 1024, 16 },
    { 641, 479, 2564, 32 },     // sizes that are not whole cells
    { 6, 12, 24, 32 },          // one cell
    { 11, 23, 48, 16 },         // one cell and some margin
    { 12, 12, 48, 32 },         // two columns, one row
    { 6, 36, 24, 16 },          // one column, three rows
  };
  UINTN  s;
  char   Line[400];

  mFbOn = FALSE;
  for (s = 0; s < ARRAY_SIZE (Shapes); s++) {
    FB      F = FbNew (Shapes[s].W, Shapes[s].H, Shapes[s].Stride, Shapes[s].Bpp);
    UINT32  Cols = Shapes[s].W / GLYPH_W, Rows = Shapes[s].H / GLYPH_H, i;

    WipeLog ();
    NewBoot ();
    TermInit (&Tm, Cols, Rows);
    SetFb (&F);
    CHECK (CURRENT->FbValid);
    CHECK (ScreenIs (&F, &Tm, FALSE));

    Say (&F, "RK3326: DRAM 1024 MB\n", "a line");
    Say (&F, "progress 10%\rprogress 99%\n", "carriage return");
    Say (&F, "tab\there, bell\a, del\x7f, high \xe9 bytes\n", "non-printing bytes");

    // Exactly to the last column, then the last row, then one more.
    for (i = 0; i < Cols && i < sizeof (Line) - 1; i++) {
      Line[i] = (char)('A' + i % 26);
    }

    Line[i] = 0;
    Say (&F, Line, "a full row");
    Say (&F, "#", "wrapped");
    while (Tm.Row < Rows - 1 || Tm.Col < Cols - 1) {
      char  C[2] = { (char)('a' + (Tm.Row + Tm.Col) % 26), 0 };
      Put (C);
      TermPut (&Tm, C[0]);
    }

    Say (&F, "$", "the last cell");
    Say (&F, "@", "one past the last cell (clear, start at the top)");
    Say (&F, "\n\n", "newlines");
    for (i = 0; i < Rows + 2; i++) {
      snprintf (Line, sizeof (Line), "line %u of a scrolling log\n", i);
      Say (&F, Line, "a screenful of lines");
    }

    // Off and on again: nothing drawn while off, the log still kept.
    RkLogScreen (FALSE);
    {
      UINT64  Before = CURRENT->Written;
      FB      Copy = FbNew (F.W, F.H, F.Stride, F.Bpp);

      memcpy (Copy.Body, F.Body, F.BodyLen);
      Put ("not on the screen\n");
      CHECK (memcmp (Copy.Body, F.Body, F.BodyLen) == 0);
      CHECK_EQ (CURRENT->Written, Before + 18);
      FbFree (&Copy);
    }

    RkLogScreen (TRUE);
    Say (&F, "visible again", "screen back on");

    // Screen log disabled at build time.
    mock_screen = FALSE;
    {
      FB  Copy = FbNew (F.W, F.H, F.Stride, F.Bpp);

      memcpy (Copy.Body, F.Body, F.BodyLen);
      Put ("pcd off\n");
      CHECK (memcmp (Copy.Body, F.Body, F.BodyLen) == 0);
      FbFree (&Copy);
    }

    mock_screen = TRUE;
    FbFree (&F);
  }

  free (Tm.Grid);
  Tm.Grid = NULL;
}

// Framebuffers the screen log must not draw on, and switching between them.
static void TestSetFramebuffer (void)
{
  FB      A = FbNew (640, 480, 2560, 32);
  FB      B = FbNew (480, 320, 960, 16);
  FB      Copy;
  UINTN   i;
  static CONST struct { CONST char *Name; UINT32 W, H, Stride, Bpp; } No[] = {
    { "24 bpp",                 640, 480, 1920, 24 },
    { "8 bpp",                  640, 480, 640,  8  },
    { "0 bpp",                  640, 480, 2560, 0  },
    { "narrower than a glyph",  GLYPH_W - 1, 480, 64, 32 },
    { "lower than a glyph",     640, GLYPH_H - 1, 2560, 32 },
    { "stride short by a byte", 640, 480, 2559, 32 },
    { "stride 0",               640, 480, 0, 16 },
  };

  mFbOn = FALSE;
  WipeLog ();
  NewBoot ();
  Put ("before any screen\n");

  // Refused shapes: nothing drawn, and the log stops drawing on the old one
  // ("anything else turns the screen log off").
  for (i = 0; i < ARRAY_SIZE (No); i++) {
    SetFb (&A);
    CHECK (CURRENT->FbValid);
    RkLogSetFramebuffer ((UINT64)(UINTN)B.Base, No[i].W, No[i].H, No[i].Stride, No[i].Bpp);
    if (CURRENT->FbValid) {
      printf ("FAIL: screen log kept for a %s framebuffer\n", No[i].Name);
    }

    CHECK (!CURRENT->FbValid);
    Copy = FbNew (A.W, A.H, A.Stride, A.Bpp);
    memcpy (Copy.Body, A.Body, A.BodyLen);
    Put ("after a refused framebuffer\n");
    CHECK (memcmp (Copy.Body, A.Body, A.BodyLen) == 0);
    FbFree (&Copy);
  }

  CHECK (FbOutsideIntact (&B));

  // A to B: B gets the newest lines, A is left alone afterwards.
  SetFb (&A);
  Put ("on A\n");
  SetFb (&B);
  CHECK (CURRENT->FbValid);
  Copy = FbNew (A.W, A.H, A.Stride, A.Bpp);
  memcpy (Copy.Body, A.Body, A.BodyLen);
  Put ("on B\n");
  CHECK (memcmp (Copy.Body, A.Body, A.BodyLen) == 0);
  CHECK (FbOutsideIntact (&B));
  FbFree (&Copy);

  // The same framebuffer again: nothing redrawn, the cursor stays.
  {
    UINT32  Row = CURRENT->Row, Col = CURRENT->Column;

    Copy = FbNew (B.W, B.H, B.Stride, B.Bpp);
    memcpy (Copy.Body, B.Body, B.BodyLen);
    SetFb (&B);
    CHECK (memcmp (Copy.Body, B.Body, B.BodyLen) == 0);
    CHECK_EQ (CURRENT->Row, Row);
    CHECK_EQ (CURRENT->Column, Col);
    FbFree (&Copy);
  }

  // The same buffer re-set with another format (a mode change in place):
  // a refused format must turn the screen log off, an accepted one must be
  // drawn with its own stride. Neither may write past the new geometry.
  {
    FB      C = FbNew (640, 480, 2560, 32);
    UINT8   *Snap = malloc (C.BodyLen);
    BOOLEAN Same24, Past16;
    size_t  End16 = (size_t)1280 * 480;

    SetFb (&C);
    Put ("32 bpp\n");
    RkLogSetFramebuffer ((UINT64)(UINTN)C.Base, 640, 480, 1920, 24);
    memcpy (Snap, C.Body, C.BodyLen);
    Put ("now 24 bpp\n");
    Same24 = memcmp (Snap, C.Body, C.BodyLen) == 0;
    KNOWN (Same24, "log-fb-same-base",
           "RkLogSetFramebuffer(same base, 640x480, 24 bpp) after 32 bpp: the log keeps drawing 32 bpp glyphs");

    WipeLog ();
    NewBoot ();
    FbFree (&C);
    C = FbNew (640, 480, 2560, 32);
    SetFb (&C);
    RkLogSetFramebuffer ((UINT64)(UINTN)C.Base, 640, 480, 1280, 16);
    memcpy (Snap, C.Body, C.BodyLen);
    for (i = 0; i < 45; i++) {
      Put ("a line on the last rows of the screen, long enough to reach the right side of it\n");
    }

    Past16 = memcmp (Snap + (C.Base - C.Body) + End16, C.Base + End16, C.BodyLen - (size_t)(C.Base - C.Body) - End16) != 0;
    KNOWN (!Past16, "log-fb-same-base",
           "RkLogSetFramebuffer(same base, 640x480, stride 1280, 16 bpp) after 32 bpp: the log writes past the "
           "16 bpp buffer's %lu bytes (old stride and depth kept)", (unsigned long)End16);
    free (Snap);
    FbFree (&C);
  }

  // A width whose byte count does not fit 32 bits must be refused, not
  // checked against a wrapped product (0x40000001 * 4 = 4).
  {
    FB  D = FbNew (640, 48, 2560, 32);

    WipeLog ();
    NewBoot ();
    RkLogSetFramebuffer ((UINT64)(UINTN)D.Base, 0x40000001, 48, 2560, 32);
    KNOWN (!CURRENT->FbValid, "log-fb-width-wrap",
           "width 0x40000001 at 32 bpp with stride 2560 accepted (Width * 4 wraps to 4); drawing would run "
           "past each line and the buffer");
    CURRENT->FbValid = 0;
    FbFree (&D);
  }

  FbFree (&A);
  FbFree (&B);
}

// A new framebuffer shows the newest part of the log again (RkLogLib.h),
// from the start of a line, ending with the newest byte.
static void TestReplay (void)
{
  static CONST struct { UINT32 W, H, Stride, Bpp; UINT32 Lines, Len; } R[] = {
    { 640, 480, 2560, 32, 500, 60  },
    { 640, 480, 1280, 16, 30,  60  },     // less than a screen
    { 480, 320, 1984, 32, 200, 150 },     // long lines that wrap
    { 640, 480, 2560, 32, 3000, 9  },     // many short lines
    { 640, 480, 2560, 32, 200, 100 },     // one row each, a screenful is 41 of them
    { 6, 12, 24, 32, 10, 5 },
    { 640, 480, 2560, 32, 0, 0 },         // empty log
    { 640, 480, 2560, 32, 1, 5000 },      // one line longer than the screen, no newline at all
  };
  static char  Log[1 << 20];
  UINTN        r;

  mFbOn = FALSE;
  for (r = 0; r < ARRAY_SIZE (R); r++) {
    FB      F = FbNew (R[r].W, R[r].H, R[r].Stride, R[r].Bpp);
    UINT32  Cols = R[r].W / GLYPH_W, Rows = R[r].H / GLYPH_H, i, j;
    size_t  N = 0, Lower, S;
    BOOLEAN Found = FALSE;
    UINT32  Shown = 0;

    WipeLog ();
    NewBoot ();
    for (i = 0; i < R[r].Lines; i++) {
      size_t  L0 = N;

      N += (size_t)snprintf (Log + N, 32, "%u:", i);
      for (j = (UINT32)(N - L0); j < R[r].Len; j++) {
        Log[N++] = (char)('a' + (i + j) % 26);
      }

      if (R[r].Len < 1000) {
        Log[N++] = '\n';
      }
    }

    SerialPortWrite ((UINT8 *)Log, N);
    SetFb (&F);
    CHECK (FbOutsideIntact (&F));

    // Some line start at or after the newest screenful matches the screen.
    Lower = N - MIN (N, (size_t)Cols * Rows);
    for (S = Lower; S <= N && !Found; S++) {
      size_t  k;

      if ((S != Lower) && (Log[S - 1] != '\n')) {
        continue;
      }

      TermInit (&Tm, Cols, Rows);
      for (k = S; k < N; k++) {
        TermPut (&Tm, Log[k]);
      }

      if (ScreenIs (&F, &Tm, TRUE)) {
        Found = TRUE;
        for (k = 0; k < (size_t)Cols * Rows; k += Cols) {
          size_t  c;

          for (c = 0; (c < Cols) && ((UINT8)Tm.Grid[k + c] <= ' '); c++) {
          }

          Shown += c < Cols;
        }
      }
    }

    if (!Found) {
      printf ("FAIL: replay %ux%u %u bpp, %u lines of %u: the screen is not the newest part of the log\n",
              R[r].W, R[r].H, R[r].Bpp, R[r].Lines, R[r].Len);
      g_fails++;
    } else if ((R[r].Lines >= Rows) && (R[r].Len < Cols)) {
      // A log with more short lines than rows: the replay should leave the
      // screen showing about a screenful, not a few lines after a clear.
      KNOWN (Shown + 1 >= Rows / 2, "replay-partial-screen",
             "%ux%u, %u lines of %u chars: only %u of %u rows show text after the replay", R[r].W, R[r].H,
             R[r].Lines, R[r].Len, Shown, Rows);
    }

    FbFree (&F);
  }

  free (Tm.Grid);
  Tm.Grid = NULL;
}

// SEC: the framebuffer U-Boot left (16/32 bpp only) gets the log from the
// first DEBUG on.
static void TestStartFramebuffer (void)
{
  static CONST UINT32  Bpp[] = { 32, 16, 24 };
  UINTN                i;

  for (i = 0; i < ARRAY_SIZE (Bpp); i++) {
    UINT32  Stride = ((640 * Bpp[i] + 31) / 32) * 4;
    FB      F = FbNew (640, 480, Stride, Bpp[i]);

    mFbOn = TRUE;
    mFb.Base = (UINT64)(UINTN)F.Base; mFb.Width = 640; mFb.Height = 480; mFb.Stride = Stride;
    mFb.BitsPerPixel = Bpp[i]; mFb.VopBase = RK3326_VOPB_BASE;
    WipeLog ();
    NewBoot ();
    TermInit (&Tm, 640 / GLYPH_W, 480 / GLYPH_H);
    if (Bpp[i] == 24) {
      UINT8  *Snap = malloc (F.BodyLen);

      CHECK (!CURRENT->FbValid);
      memcpy (Snap, F.Body, F.BodyLen);
      Put ("Project Silicium for r36s\n");
      CHECK (memcmp (Snap, F.Body, F.BodyLen) == 0);
      free (Snap);
    } else {
      CHECK (CURRENT->FbValid);
      Say (&F, "Project Silicium for r36s\n", "the first line in SEC");
    }

    FbFree (&F);
  }

  mFbOn = FALSE;
  free (Tm.Grid);
  Tm.Grid = NULL;
}

int main (void)
{
  void  *Lo, *Log, *Hi;

  Lo  = mmap ((void *)(RK_LOG_BASE - 0x1000), 0x1000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  Log = mmap ((void *)RK_LOG_BASE, RK_LOG_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  Hi  = mmap ((void *)(RK_LOG_BASE + RK_LOG_SIZE), 0x1000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if ((Lo != (void *)(RK_LOG_BASE - 0x1000)) || (Log != (void *)RK_LOG_BASE) || (Hi != (void *)(RK_LOG_BASE + RK_LOG_SIZE))) {
    printf ("FAIL: cannot map the log at 0x%x on this host\n", RK_LOG_BASE);
    return 1;
  }

  TestRing ();
  TestPrevious ();
  TestUart ();
  TestFont ();
  TestScreen ();
  TestSetFramebuffer ();
  TestReplay ();
  TestStartFramebuffer ();
  DONE_KNOWN ();
}

// RkI2cLib.c against a model of the RK3x I2C controller. The model follows the
// registers the way U-Boot's driver and the Linux driver use them; it is only as
// good as that reading of the hardware, but it checks the sequencing, packing,
// chunking and error paths.
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/CruLib.h>
#include <Library/GpioLib.h>
#include <Library/RkI2cLib.h>
#include <string.h>
#include "test.h"

#define BASE 0xFF180000UL
static UINT32 reg[0x400 / 4];          // controller registers
static UINT8  dev[256];                // the "PMIC" register file at 0x20
static UINT8  nack_all, hang;
static int    last_chunk_lastack_errors, chunk_too_big, calls_start, calls_stop;
static unsigned rx_off;                // read position inside a multi-chunk read
static UINT8 cur_reg;

static int pin_fn[4][32];

VOID CruEnableGpioClocks (VOID) {}
UINT32 CruEnableI2cClock (UINT8 Bus) { return 24000000; }
VOID GpioPinSetFunction (UINT8 g, UINT8 p, UINT8 f) { pin_fn[g][p] = f; }
VOID GpioPinSetPull (UINT8 g, UINT8 p, GPIO_PIN_PULL pull) {}
VOID GpioPinSetInput (UINT8 g, UINT8 p, GPIO_PIN_INPUT_ENABLE e) {}

#define R(off) reg[((off)) / 4]

// An event happens a little after the command that causes it, and shows in IPD
// only if its interrupt is enabled by then: latch it now, expose it on the
// next IPD read.
static UINT32 latched;

static void fire (UINT32 bit)
{
  if (hang) return;
  latched |= bit;
}

UINT32 EFIAPI MmioRead32 (UINTN a)
{
  UINTN o = a - BASE;
  if (o == 0x1C) {
    R (0x1C) |= latched & R (0x18);
    latched &= ~R (0x18);
  }
  return R (o);
}

UINT32 EFIAPI MmioWrite32 (UINTN a, UINT32 v)
{
  UINTN o = a - BASE;

  switch (o) {
    case 0x1C:                              // IPD: write 1 to clear
      R (0x1C) &= ~v;
      latched &= ~v;
      return v;
    case 0x00:                              // CON
      R (0x00) = v;
      if (v & (1 << 3)) { calls_start++; fire (1 << 4); }
      if (v & (1 << 4)) { calls_stop++; fire (1 << 5); }
      return v;
    case 0x14: {                            // MRXCNT: start a read chunk
      UINT32 n = v, con = R (0x00);
      R (0x14) = v;
      if (n > 32) chunk_too_big++;
      if (((con >> 1) & 3) == 1) {          // TRX: address phase first
        UINT32 ma = R (0x08), mr = R (0x0C);
        if (!(ma & (1u << 24)) || !(mr & (1u << 24)) || (ma & 1) != 1) { fire (1 << 6); return v; }
        if (nack_all || ((ma >> 1) & 0x7F) != 0x20) { fire (1 << 6); return v; }
        cur_reg = mr & 0xFF;
        rx_off = 0;
      } else if (((con >> 1) & 3) != 2) {
        fire (1 << 6); return v;
      }
      // LASTACK (bit 5) is for the last chunk only; the lib sets it when n < remaining.
      (void)last_chunk_lastack_errors;
      for (UINT32 i = 0; i < n; i++) {
        UINT32 w = 0x200 / 4 + (i / 4);
        if (i % 4 == 0) reg[w] = 0;
        reg[w] |= (UINT32)dev[(UINT8)(cur_reg + rx_off + i)] << ((i % 4) * 8);
      }
      rx_off += n;
      fire (1 << 3);
      return v;
    }
    case 0x10: {                            // MTXCNT: a write transfer
      UINT32 n = v;
      R (0x10) = v;
      UINT8 b[32];
      for (UINT32 i = 0; i < n; i++) b[i] = reg[0x100 / 4 + i / 4] >> ((i % 4) * 8);
      if (nack_all || (b[0] >> 1) != 0x20 || (b[0] & 1)) { fire (1 << 6); return v; }
      for (UINT32 i = 2; i < n; i++) dev[(UINT8)(b[1] + i - 2)] = b[i];
      fire (1 << 2);
      return v;
    }
    default:
      R (o) = v;
      return v;
  }
}

int main (void)
{
  UINT8 out[64];
  UINT8 data[40];

  for (int i = 0; i < 256; i++) dev[i] = (UINT8)(i * 7 + 3);
  dev[0xED] = 0x81; dev[0xEE] = 0x70;

  CHECK_EQ (RkI2cInit (0, 400000), EFI_SUCCESS);
  CHECK_EQ (R (0x04), (3u << 16) | 3u);
  CHECK_EQ (pin_fn[0][8], 1);                // GPIO0_B0
  CHECK_EQ (pin_fn[0][9], 1);                // GPIO0_B1
  CHECK_EQ (RkI2cInit (4, 400000), EFI_INVALID_PARAMETER);
  CHECK_EQ (RkI2cInit (2, 400000), EFI_UNSUPPORTED);

  // single byte, the chip id
  CHECK_EQ (RkI2cRead (0, 0x20, 0xED, out, 1), EFI_SUCCESS);
  CHECK_EQ (out[0], 0x81);
  CHECK_EQ (RkI2cRead (0, 0x20, 0xED, out, 2), EFI_SUCCESS);
  CHECK_EQ (out[0], 0x81); CHECK_EQ (out[1], 0x70);
  CHECK_EQ (R (0x00), 0);                    // controller disabled again
  CHECK (calls_start >= 2 && calls_stop >= 2);

  // longer than the 32 byte FIFO: two chunks, sequential bytes
  CHECK_EQ (RkI2cRead (0, 0x20, 0x10, out, 40), EFI_SUCCESS);
  for (int i = 0; i < 40; i++) CHECK_EQ (out[i], (UINT8)((0x10 + i) * 7 + 3));
  CHECK_EQ (chunk_too_big, 0);
  CHECK_EQ (RkI2cRead (0, 0x20, 0x00, out, 33), EFI_SUCCESS);
  for (int i = 0; i < 33; i++) CHECK_EQ (out[i], (UINT8)(i * 7 + 3));
  // odd sizes pack right
  CHECK_EQ (RkI2cRead (0, 0x20, 0x41, out, 5), EFI_SUCCESS);
  for (int i = 0; i < 5; i++) CHECK_EQ (out[i], (UINT8)((0x41 + i) * 7 + 3));

  // writes: address byte, register, data
  data[0] = 0xAB;
  CHECK_EQ (RkI2cWrite (0, 0x20, 0xF4, data, 1), EFI_SUCCESS);
  CHECK_EQ (dev[0xF4], 0xAB);
  data[0] = 1; data[1] = 2; data[2] = 3; data[3] = 4; data[4] = 5;
  CHECK_EQ (RkI2cWrite (0, 0x20, 0x60, data, 5), EFI_SUCCESS);
  for (int i = 0; i < 5; i++) CHECK_EQ (dev[0x60 + i], i + 1);
  CHECK_EQ (RkI2cWrite (0, 0x20, 0x70, data, 30), EFI_SUCCESS);      // 32 bytes on the wire: the FIFO
  CHECK_EQ (RkI2cWrite (0, 0x20, 0x70, data, 31), EFI_INVALID_PARAMETER);

  // NAK: wrong chip, then the bus still works
  CHECK_EQ (RkI2cRead (0, 0x21, 0xED, out, 1), EFI_NO_RESPONSE);
  CHECK_EQ (RkI2cWrite (0, 0x21, 0x10, data, 1), EFI_NO_RESPONSE);
  CHECK_EQ (R (0x00), 0);
  CHECK_EQ (RkI2cRead (0, 0x20, 0xED, out, 1), EFI_SUCCESS);
  CHECK_EQ (out[0], 0x81);

  // controller never answers: times out instead of hanging
  hang = 1;
  CHECK_EQ (RkI2cRead (0, 0x20, 0xED, out, 1), EFI_TIMEOUT);
  CHECK_EQ (RkI2cWrite (0, 0x20, 0x10, data, 1), EFI_TIMEOUT);
  hang = 0;

  CHECK_EQ (RkI2cRead (0, 0x20, 0, NULL, 1), EFI_INVALID_PARAMETER);
  CHECK_EQ (RkI2cRead (0, 0x20, 0, out, 0), EFI_INVALID_PARAMETER);
  CHECK_EQ (RkI2cRead (9, 0x20, 0, out, 1), EFI_INVALID_PARAMETER);
  DONE ();
}

// DesignWare MSHC driver arithmetic: clock divider, command register bits,
// FIFO thresholds and the IDMAC descriptor chain. Expected values come from
// the Linux dw_mmc driver, the DesignWare databook field layout and the SD/MMC
// command set, not from the code under test.
#include <Uefi.h>
#include <string.h>
#include "test.h"
#include "DwMmcMath.h"

static void test_divider (void)
{
  UINT32 act;

  // PX30: controller input 50 MHz. Div 0 = pass-through, else base / (2 * div).
  CHECK_EQ (DwMmcCalcClockDiv (50000, 50000, &act), 0);  CHECK_EQ (act, 50000);
  CHECK_EQ (DwMmcCalcClockDiv (50000, 52000, &act), 0);  CHECK_EQ (act, 50000);
  CHECK_EQ (DwMmcCalcClockDiv (50000, 25000, &act), 1);  CHECK_EQ (act, 25000);
  CHECK_EQ (DwMmcCalcClockDiv (50000, 26000, &act), 1);  CHECK_EQ (act, 25000);
  CHECK_EQ (DwMmcCalcClockDiv (50000, 20000, &act), 2);  CHECK_EQ (act, 12500);
  // identification clock: 400 kHz -> div 63 -> 396.8 kHz, never above 400
  CHECK_EQ (DwMmcCalcClockDiv (50000, 400, &act), 63);   CHECK_EQ (act, 396);
  CHECK (act <= 400);
  // floor of the 8-bit register
  CHECK_EQ (DwMmcCalcClockDiv (50000, 10, &act), 255);   CHECK_EQ (act, 98);
  // odd base clock: result never exceeds the target
  for (UINT32 base = 24000; base <= 150000; base += 1337) {
    for (UINT32 t = 100; t <= 60000; t += 997) {
      UINT32 d = DwMmcCalcClockDiv (base, t, &act);
      if (d < 255 && t < base) CHECK (act <= t);
      CHECK (d <= 255);
    }
  }
  // same answer as the kernel's DIV_ROUND_UP (bus_hz, 2 * clock)
  CHECK_EQ (DwMmcCalcClockDiv (150000, 400, NULL), (150000 + 799) / 800 > 255 ? 255 : (150000 + 799) / 800);
}

static void test_cmd_flags (void)
{
  UINT32 c;

  // CMD0: no response, initialisation clocks
  c = DwMmcBuildCmd (0, SdMmcCommandTypeBc, 0, DwMmcDataNone);
  CHECK_EQ (c & 0x3f, 0);
  CHECK (c & BIT_CMD_SEND_INIT);
  CHECK (!(c & BIT_CMD_RESPONSE_EXPECT));
  CHECK (c & BIT_CMD_USE_HOLD_REG);

  // CMD2: long response with CRC check
  c = DwMmcBuildCmd (2, SdMmcCommandTypeBcr, SdMmcResponseTypeR2, DwMmcDataNone);
  CHECK_EQ (c & (BIT_CMD_RESPONSE_EXPECT | BIT_CMD_LONG_RESPONSE | BIT_CMD_CHECK_RESPONSE_CRC),
            BIT_CMD_RESPONSE_EXPECT | BIT_CMD_LONG_RESPONSE | BIT_CMD_CHECK_RESPONSE_CRC);

  // ACMD41 / CMD1: R3 has no CRC
  c = DwMmcBuildCmd (41, SdMmcCommandTypeBcr, SdMmcResponseTypeR3, DwMmcDataNone);
  CHECK (c & BIT_CMD_RESPONSE_EXPECT);
  CHECK (!(c & BIT_CMD_CHECK_RESPONSE_CRC));
  CHECK (!(c & BIT_CMD_LONG_RESPONSE));

  // CMD8 (R7), CMD3 (R6), CMD13 (R1), CMD6 on eMMC (R1b): short + CRC
  UINT32 types[] = { SdMmcResponseTypeR7, SdMmcResponseTypeR6, SdMmcResponseTypeR1, SdMmcResponseTypeR1b };
  for (int i = 0; i < 4; i++) {
    c = DwMmcBuildCmd (8, SdMmcCommandTypeBcr, types[i], DwMmcDataNone);
    CHECK_EQ (c & (BIT_CMD_RESPONSE_EXPECT | BIT_CMD_LONG_RESPONSE | BIT_CMD_CHECK_RESPONSE_CRC),
              BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC);
  }

  // CMD17 single block read: data, wait for previous data, no auto stop
  c = DwMmcBuildCmd (17, SdMmcCommandTypeAdtc, SdMmcResponseTypeR1, DwMmcDataRead);
  CHECK (c & BIT_CMD_DATA_EXPECTED);
  CHECK (c & BIT_CMD_WAIT_PRVDATA_COMPLETE);
  CHECK (!(c & BIT_CMD_WRITE));
  CHECK (!(c & BIT_CMD_SEND_AUTO_STOP));

  // CMD18 / CMD25: open ended, the host has to stop them
  c = DwMmcBuildCmd (18, SdMmcCommandTypeAdtc, SdMmcResponseTypeR1, DwMmcDataRead);
  CHECK (c & BIT_CMD_SEND_AUTO_STOP);
  CHECK (!(c & BIT_CMD_WRITE));
  c = DwMmcBuildCmd (25, SdMmcCommandTypeAdtc, SdMmcResponseTypeR1, DwMmcDataWrite);
  CHECK (c & BIT_CMD_SEND_AUTO_STOP);
  CHECK (c & BIT_CMD_WRITE);

  // CMD6 / CMD51 / CMD8(ext csd) data commands never auto stop
  c = DwMmcBuildCmd (6, SdMmcCommandTypeAdtc, SdMmcResponseTypeR1, DwMmcDataRead);
  CHECK (!(c & BIT_CMD_SEND_AUTO_STOP));
  // CMD12: abort flag, does not wait for data
  c = DwMmcBuildCmd (12, SdMmcCommandTypeAc, SdMmcResponseTypeR1b, DwMmcDataNone);
  CHECK (c & BIT_CMD_STOP_ABORT_CMD);
  CHECK (!(c & BIT_CMD_WAIT_PRVDATA_COMPLETE));
  // plain commands do not touch the data wait bit
  c = DwMmcBuildCmd (13, SdMmcCommandTypeAc, SdMmcResponseTypeR1, DwMmcDataNone);
  CHECK (!(c & BIT_CMD_WAIT_PRVDATA_COMPLETE));
  CHECK (!(c & BIT_CMD_DATA_EXPECTED));
  // the bits sit where the databook puts them
  CHECK_EQ (BIT_CMD_START, 0x80000000u);
  CHECK_EQ (BIT_CMD_USE_HOLD_REG, 0x20000000);
  CHECK_EQ (BIT_CMD_UPDATE_CLOCK_ONLY, 0x00200000);
  CHECK_EQ (BIT_CMD_SEND_AUTO_STOP, 0x1000);
  CHECK_EQ (BIT_CMD_WAIT_PRVDATA_COMPLETE, 0x2000);
  CHECK_EQ (BIT_CMD_DATA_EXPECTED, 0x200);
}

static void test_fifoth (void)
{
  // kernel with fifo-depth 0x100, 512 byte blocks: msize 6 (128), rx 127, tx 128
  CHECK_EQ (DwMmcCalcFifoth (512, TRUE),  (6u << 28) | (127u << 16) | 128u);
  // 64 byte block (16 words): burst 16 -> msize 3, rx 15
  CHECK_EQ (DwMmcCalcFifoth (64, TRUE),   (3u << 28) | (15u << 16) | 128u);
  // 8 byte: 2 words, only single transfers
  CHECK_EQ (DwMmcCalcFifoth (8, TRUE),    (0u << 28) | (1u << 16) | 128u);
  // by hand: half FIFO watermarks, single bursts
  CHECK_EQ (DwMmcCalcFifoth (512, FALSE), (0u << 28) | (127u << 16) | 128u);
}

static void check_chain (UINT64 data, UINT64 len, UINT32 want)
{
  static DW_MMC_HC_DMA_DESC_LINE d[DW_MMC_IDMAC_MAX_DESC];
  UINT32 dp = 0x40000000, n, i;
  UINT64 sum = 0;

  memset (d, 0xAA, sizeof d);
  n = DwMmcFillDescriptors (d, DW_MMC_IDMAC_MAX_DESC, dp, data, len);
  CHECK_EQ (n, want);
  if (n == 0) return;
  for (i = 0; i < n; i++) {
    UINT32 sz = d[i].Des1 & 0x1fff;
    CHECK (d[i].Des0 & DW_MMC_IDMAC_DES0_OWN);
    CHECK_EQ (d[i].Des2, (UINT32)(data + sum));
    CHECK ((sz & 3) == 0 && sz > 0 && sz <= 4096);
    sum += sz;
    if (i + 1 < n) {
      CHECK (d[i].Des0 & DW_MMC_IDMAC_DES0_CH);
      CHECK (d[i].Des0 & DW_MMC_IDMAC_DES0_DIC);
      CHECK (!(d[i].Des0 & DW_MMC_IDMAC_DES0_LD));
      CHECK_EQ (d[i].Des3, dp + (i + 1) * 16);
      CHECK_EQ (sz, 4096);
    }
  }
  CHECK_EQ (sum, len);
  CHECK ((d[0].Des0 & DW_MMC_IDMAC_DES0_FS) != 0);
  for (i = 1; i < n; i++) CHECK (!(d[i].Des0 & DW_MMC_IDMAC_DES0_FS));
  CHECK ((d[n - 1].Des0 & DW_MMC_IDMAC_DES0_LD) != 0);
  CHECK (!(d[n - 1].Des0 & DW_MMC_IDMAC_DES0_CH));
  CHECK (!(d[n - 1].Des0 & DW_MMC_IDMAC_DES0_DIC));   // completion interrupt status on the last
  CHECK_EQ (d[n - 1].Des3, 0);
}

static void test_descriptors (void)
{
  DW_MMC_HC_DMA_DESC_LINE d[2];

  check_chain (0x02000000, 512, 1);
  check_chain (0x02000000, 4096, 1);
  check_chain (0x02000000, 4100, 2);                       // 4096 + 4
  check_chain (0x02000004, 8192, 2);
  check_chain (0x02000000, 512 * 40000, 5000);             // 19.5 MiB, the kernel
  check_chain (0x02000000, 20u * 1024 * 1024, 5120);       // exactly 20 MiB
  check_chain (0x02000000, 0xFFFFull * 512, 8192);         // the biggest SdDxe asks for
  check_chain (0x02000000, 32u * 1024 * 1024, 8192);
  check_chain (0x02000000, 32u * 1024 * 1024 + 4, 0);      // one too many
  check_chain (0x02000001, 512, 0);                        // not word aligned
  check_chain (0x02000000, 513, 0);
  check_chain (0x02000000, 0, 0);
  check_chain (0xFFFFF000ull, 8192, 0);                    // runs past 4 GiB
  check_chain (0xFFFFF000ull, 4096, 1);                    // ends exactly at 4 GiB
  // the array is not touched when it is refused
  memset (d, 0x55, sizeof d);
  CHECK_EQ (DwMmcFillDescriptors (d, 1, 0, 0x1000, 8192), 0);
  CHECK_EQ (d[0].Des0, 0x55555555u);
}

static void test_reachable (void)
{
  CHECK (DwMmcDmaReachable (0x02000000, 512));
  CHECK (!DwMmcDmaReachable (0x02000000, 0));
  CHECK (!DwMmcDmaReachable (0x02000002, 512));
  CHECK (!DwMmcDmaReachable (0x02000000, 510));
  CHECK (DwMmcDmaReachable (0xFFFFF000ull, 4096));
  CHECK (!DwMmcDmaReachable (0xFFFFF000ull, 4100));
  CHECK (!DwMmcDmaReachable (0x1FFFF0000ull, 512));
  CHECK (DwMmcDmaReachable (0, 32u * 1024 * 1024));
  CHECK (!DwMmcDmaReachable (0, 32u * 1024 * 1024 + 4));
}

int main (void)
{
  test_divider ();
  test_cmd_flags ();
  test_fifoth ();
  test_descriptors ();
  test_reachable ();
  DONE ();
}

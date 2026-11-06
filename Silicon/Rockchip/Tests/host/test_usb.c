// USB host port: FIFO sizing, the 5 V policy, the SoC bring-up sequence
// (RkUsbPlatformLib against a model of PMU/CRU/GRF) and the board device
// trees (which pin gets driven, whether the boost runs, what a charger does).
#include <Uefi.h>
#include <Library/CruLib.h>
#include <Library/FdtLib.h>
#include <Library/GpioLib.h>
#include <Library/RkPmicLib.h>
#include <Library/RkUsbPlatformLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include "test.h"
#include "DwUsbFifo.h"

extern UINTN mock_delay_us;
extern int mock_verbose;

// ---- register model -------------------------------------------------------

static UINT32 pmu[0x1000 / 4], cru[0x500 / 4], pmucru[0x100 / 4], grf[0x10000 / 4], dwc[0x100 / 4];
static int pmu_stuck_off, no_core;
static int mmio_log_n;
static struct { UINTN a; UINT32 v; } mmio_log[512];

static UINT32 *reg (UINTN a)
{
  if (a >= 0xFF000000UL && a < 0xFF001000UL) return &pmu[(a - 0xFF000000UL) / 4];
  if (a >= CRU_BASE && a < CRU_BASE + 0x500) return &cru[(a - CRU_BASE) / 4];
  if (a >= PMUCRU_BASE && a < PMUCRU_BASE + 0x100) return &pmucru[(a - PMUCRU_BASE) / 4];
  if (a >= 0xFF2C0000UL && a < 0xFF2D0000UL) return &grf[(a - 0xFF2C0000UL) / 4];
  if (a >= 0xFF300000UL && a < 0xFF300100UL) return &dwc[(a - 0xFF300000UL) / 4];
  printf ("FAIL: stray access 0x%lx\n", (unsigned long)a);
  exit (1);
}

UINT32 EFIAPI MmioRead32 (UINTN a) { return *reg (a); }

UINT32 EFIAPI MmioWrite32 (UINTN a, UINT32 v)
{
  UINT32 *r = reg (a);
  mmio_log[mmio_log_n].a = a; mmio_log[mmio_log_n].v = v; mmio_log_n++;
  if (a >= 0xFF000000UL && a < 0xFF001000UL) {
    // PMU registers are write-enable masked too; power-down status follows
    // PWRDN_CON unless the domain is stuck.
    UINT32 m = v >> 16;
    *r = (*r & ~m) | (v & m & 0xFFFF);
    if (a == 0xFF000018UL && !pmu_stuck_off) {
      pmu[0x20 / 4] = (pmu[0x20 / 4] & ~(1U << 5)) | (*r & (1U << 5));
    }
    if (a == 0xFF000064UL) {
      pmu[0x6C / 4] = (pmu[0x6C / 4] & ~(1U << 10)) | (*r & (1U << 10));
    }
  } else if ((a >= CRU_BASE + 0x100 && a < CRU_BASE + 0x500) || (a >= PMUCRU_BASE + 0x40 && a < PMUCRU_BASE + 0x100) ||
             (a >= 0xFF2C0000UL && a < 0xFF2D0000UL)) {
    UINT32 m = v >> 16;
    *r = (*r & ~m) | (v & m & 0xFFFF);
  } else {
    *r = v;
  }
  return v;
}

// ---- other libraries the lib calls -----------------------------------------

static int  gpio_func[5][32], gpio_dir[5][32], gpio_val[5][32], gpio_in[5][32];
static int  gpio_log_n;
static struct { int bank, pin, val; } gpio_log[64];

void EFIAPI GpioPinSetDirection (UINT8 G, UINT8 P, GPIO_PIN_DIRECTION D) { gpio_dir[G][P] = D; }
void EFIAPI GpioPinWrite (UINT8 G, UINT8 P, BOOLEAN V) { gpio_val[G][P] = V; gpio_log[gpio_log_n].bank = G; gpio_log[gpio_log_n].pin = P; gpio_log[gpio_log_n].val = V; gpio_log_n++; }
BOOLEAN EFIAPI GpioPinGetLevel (UINT8 G, UINT8 P) { return gpio_dir[G][P] == GPIO_PIN_OUTPUT ? gpio_val[G][P] : gpio_in[G][P]; }
void EFIAPI GpioPinSetFunction (UINT8 G, UINT8 P, UINT8 F) { gpio_func[G][P] = F; }

static int charger_pmic, boost_calls_on, boost_calls_off;
EFI_STATUS EFIAPI RkPmicGetPowerStatus (OUT RK_POWER_STATUS *S) { memset (S, 0, sizeof (*S)); S->ChargerPresent = charger_pmic; return EFI_SUCCESS; }
EFI_STATUS EFIAPI RkPmicSetRegulator (IN CONST VOID *Fdt, IN INT32 Node, IN BOOLEAN En)
{
  const char *n = FdtGetName (Fdt, Node, NULL);
  CHECK (strcmp (n, "BOOST") == 0 || strcmp (n, "boost") == 0 || 1);
  if (En) boost_calls_on++; else boost_calls_off++;
  return EFI_SUCCESS;
}
INTN EFIAPI AsciiStrCmp (IN CONST CHAR8 *a, IN CONST CHAR8 *b) { return strcmp ((const char *)a, (const char *)b); }

// GSNPSID is read-only in the model
static void fresh (void)
{
  memset (pmu, 0, sizeof (pmu)); memset (cru, 0, sizeof (cru)); memset (pmucru, 0, sizeof (pmucru));
  memset (grf, 0, sizeof (grf)); memset (dwc, 0, sizeof (dwc));
  dwc[0x40 / 4] = no_core ? 0xFFFFFFFF : 0x4F54330A;
  // everything gated, domain off, as a cold boot ROM might leave it
  cru[(0x200 + 7 * 4) / 4] = 0xFFFF; cru[(0x200 + 16 * 4) / 4] = 0xFFFF; pmucru[(0x80 + 4) / 4] = 0xFFFF;
  pmu[0x20 / 4] = 1U << 5;
  mmio_log_n = 0; mock_delay_us = 0;
}

static int wrote (UINTN a, UINT32 v)
{
  for (int i = 0; i < mmio_log_n; i++) if (mmio_log[i].a == a && mmio_log[i].v == v) return 1;
  return 0;
}

static int log_index (UINTN a, UINT32 v)
{
  for (int i = 0; i < mmio_log_n; i++) if (mmio_log[i].a == a && mmio_log[i].v == v) return i;
  return -1;
}

// ---- tests ------------------------------------------------------------------

static void test_fifo (void)
{
  DW_FIFO_LAYOUT L;

  CHECK (!DwUsbSizeFifos (0, &L));
  CHECK (!DwUsbSizeFifos (223, &L));
  CHECK (DwUsbSizeFifos (224, &L));
  CHECK_EQ (L.RxDepth, DW_FIFO_MIN_RX); CHECK_EQ (L.NpTxDepth, 32); CHECK_EQ (L.PTxDepth, 32);

  CHECK (DwUsbSizeFifos (1024, &L));       // fits: wanted sizes
  CHECK_EQ (L.RxDepth, 525); CHECK_EQ (L.NpTxDepth, 128); CHECK_EQ (L.PTxDepth, 256);
  CHECK_EQ (L.NpTxStart, 525); CHECK_EQ (L.PTxStart, 653);

  CHECK (DwUsbSizeFifos (768, &L));        // 141 words over: the periodic FIFO pays
  CHECK_EQ (L.RxDepth, 525); CHECK_EQ (L.NpTxDepth, 128); CHECK_EQ (L.PTxDepth, 115);

  CHECK (DwUsbSizeFifos (400, &L));        // both TX at minimum, rx gets the rest
  CHECK_EQ (L.PTxDepth, 32); CHECK_EQ (L.NpTxDepth, 32); CHECK_EQ (L.RxDepth, 336);

  for (UINT32 total = 0; total < 9000; total++) {
    int ok = DwUsbSizeFifos (total, &L);
    CHECK_EQ (ok, total >= 224);
    if (!ok) continue;
    CHECK (L.RxDepth >= DW_FIFO_MIN_RX && L.NpTxDepth >= DW_FIFO_MIN_NPTX && L.PTxDepth >= DW_FIFO_MIN_PTX);
    CHECK (L.RxDepth <= DW_FIFO_WANT_RX && L.NpTxDepth <= DW_FIFO_WANT_NPTX && L.PTxDepth <= DW_FIFO_WANT_PTX);
    CHECK_EQ (L.NpTxStart, L.RxDepth);
    CHECK_EQ (L.PTxStart, L.RxDepth + L.NpTxDepth);
    CHECK (L.PTxStart + L.PTxDepth <= total);
  }
}

#include "../../RK3326Pkg/Library/RkUsbPlatformLib/UsbVbusPolicy.h"

static void test_policy (void)
{
  for (int shared = 0; shared < 2; shared++) {
    for (int chg = 0; chg < 2; chg++) {
      USB_VBUS_INPUT in = { shared, chg };
      CHECK_EQ (UsbVbusDecide (&in), (shared && chg) ? UsbVbusCharge : UsbVbusHost);
    }
  }
}

static void test_poweron (void)
{
  UINT64 base = 0;

  no_core = 0; pmu_stuck_off = 0; fresh ();
  CHECK_EQ (RkUsbPlatformPowerOn (&base), EFI_SUCCESS);
  CHECK_EQ (base, 0xFF300000ULL);
  CHECK_EQ (pmu[0x20 / 4] & (1U << 5), 0);                         // PD_USB on
  CHECK_EQ (cru[(0x200 + 7 * 4) / 4] & 0x3C, 0);                   // hclk_usb, adp, niu, hclk_otg ungated
  CHECK_EQ (cru[(0x200 + 7 * 4) / 4] & ~0x3C, 0xFFFF & ~0x3C);     // nothing else touched
  CHECK_EQ (cru[(0x200 + 16 * 4) / 4] & (1U << 6), 0);             // pclk_usb_grf
  CHECK_EQ (pmucru[(0x80 + 4) / 4] & (1U << 9), 0);                // clk_usbphy_ref
  CHECK_EQ ((grf[0x100 / 4] >> 0) & 0x1FF, 0);                     // PHY not suspended
  CHECK_EQ (grf[0x100 / 4] & (1U << 10), 0);                       // iddig low: host
  CHECK_EQ (grf[0x100 / 4] & (1U << 9), 1U << 9);                  // GRF drives iddig
  CHECK_EQ (grf[0x108 / 4] & (1U << 4), 0);                        // 480 MHz clock out on
  CHECK_EQ (grf[0x1C / 4] & (1U << 15), 1U << 15);                 // vbus detect powered down
  CHECK_EQ (grf[0x2C / 4], 0x0400);                                // tuning registers
  CHECK_EQ (grf[0x00 / 4], 0x0004); CHECK_EQ (grf[0x30 / 4], 0x0004);
  // the OTG resets were pulsed (assert then deassert), after the PHY woke up
  int a = log_index (CRU_BASE + 0x300 + 4 * 4, (1U << 19) | (1U << 3));
  int d = log_index (CRU_BASE + 0x300 + 4 * 4, (1U << 19));
  int p = log_index (0xFF2C0100UL, 0x01FF0000U);
  CHECK (a >= 0 && d > a && p >= 0 && p < a);
  // PD up before any register of the block is touched
  CHECK (log_index (0xFF000018UL, (1U << 21)) >= 0);
  CHECK (log_index (0xFF000018UL, (1U << 21)) < log_index (CRU_BASE + 0x200 + 7 * 4, 0x003C0000U));

  // already on: no PMU power write
  fresh (); pmu[0x20 / 4] = 0;
  CHECK_EQ (RkUsbPlatformPowerOn (&base), EFI_SUCCESS);
  CHECK (!wrote (0xFF000018UL, (1U << 21)));

  // domain never comes up: error, bounded
  fresh (); pmu_stuck_off = 1;
  CHECK_EQ (RkUsbPlatformPowerOn (&base), EFI_TIMEOUT);
  CHECK (mock_delay_us <= 11000);

  // no DWC2 behind it
  pmu_stuck_off = 0; no_core = 1; fresh ();
  CHECK_EQ (RkUsbPlatformPowerOn (&base), EFI_DEVICE_ERROR);
  no_core = 0;
  CHECK_EQ (RkUsbPlatformPowerOn (NULL), EFI_INVALID_PARAMETER);
}

// ---- board device trees -----------------------------------------------------

static void *load (const char *path, long *size)
{
  FILE *f = fopen (path, "rb");
  if (!f) return NULL;
  fseek (f, 0, SEEK_END); *size = ftell (f); fseek (f, 0, SEEK_SET);
  void *b = malloc (*size);
  if (fread (b, 1, *size, f) != (size_t)*size) { free (b); b = NULL; }
  fclose (f);
  return b;
}

static void clear_actions (void)
{
  memset (gpio_val, 0, sizeof (gpio_val)); memset (gpio_dir, 0, sizeof (gpio_dir)); memset (gpio_in, 0, sizeof (gpio_in));
  gpio_log_n = 0; boost_calls_on = boost_calls_off = 0; charger_pmic = 0;
}

// Returns a one-word summary of what a device tree does with the port.
static void run_dtb (const char *path, int pin_chg, int pmic_chg, char *out, EFI_STATUS *st)
{
  long n; void *fdt = load (path, &n);
  if (!fdt) { strcpy (out, "unreadable"); return; }
  clear_actions ();
  gpio_in[0][11] = pin_chg; gpio_in[3][5] = pin_chg;   // dc_det: GPIO0_B3 on most boards, GPIO3_A5 on the D-28S
  charger_pmic = pmic_chg;
  *st = RkUsbPlatformApplyFdt (fdt);
  int host_gpio = -1, host_bank = -1;
  for (int i = 0; i < gpio_log_n; i++) { host_bank = gpio_log[i].bank; host_gpio = gpio_log[i].pin; }
  sprintf (out, "gpio %d_%d=%d boost on/off %d/%d delay %luus", host_bank, host_gpio,
           host_gpio >= 0 ? gpio_val[host_bank][host_gpio] : -1, boost_calls_on, boost_calls_off, (unsigned long)mock_delay_us);
  free (fdt);
}

static void test_boards (const char *dir)
{
  char path[512], out[200];
  EFI_STATUS st;
  const char *root = dir;

  // no DTB: nothing switched
  clear_actions ();
  CHECK_EQ (RkUsbPlatformApplyFdt (NULL), EFI_SUCCESS);
  CHECK_EQ (gpio_log_n, 0);

  // every compiled board: never crashes, never charge mode without the DT
  // flags, and the host GPIO (if any) ends up driven to its active level
  {
    DIR *d = opendir (root);
    struct dirent *e;
    int n = 0, vcc = 0, shared = 0;
    while (d && (e = readdir (d))) {
      if (strncmp (e->d_name, "rk3326-r36s-", 12) != 0 || strstr (e->d_name, ".dtb") == NULL) continue;
      snprintf (path, sizeof (path), "%s/%s", root, e->d_name);
      for (int chg = 0; chg < 2; chg++) {
        mock_delay_us = 0; run_dtb (path, chg, chg, out, &st);
        CHECK (st == EFI_SUCCESS || st == EFI_NOT_READY);
        if (st == EFI_NOT_READY) CHECK (mock_delay_us >= 150000);
      }
      run_dtb (path, 0, 0, out, &st);
      n++; vcc += gpio_log_n > 0; shared += boost_calls_off > 0;
    }
    if (d) closedir (d);
    printf ("  %d board DTBs: %d with a vcc_host GPIO, %d on a shared port\n", n, vcc, shared);
    CHECK (n == 0 || n > 20);
  }

  // genuine R36S: no vcc_host, no boost-only: nothing to switch
  snprintf (path, sizeof (path), "%s/rk3326-r36s-android-panel0.dtb", root);
  mock_delay_us = 0; run_dtb (path, 0, 0, out, &st);
  printf ("  R36S:        %s\n", out); CHECK_EQ (st, EFI_SUCCESS); CHECK_EQ (gpio_log_n, 0); CHECK_EQ (boost_calls_on + boost_calls_off, 0);
  mock_delay_us = 0; run_dtb (path, 1, 1, out, &st);          // charger in: still host
  CHECK_EQ (st, EFI_SUCCESS); CHECK_EQ (mock_delay_us, 0);

  // EE clone with vcc_host on GPIO3_A4
  snprintf (path, sizeof (path), "%s/rk3326-r36s-clone-g80-panel8.dtb", root);
  mock_delay_us = 0; run_dtb (path, 0, 0, out, &st);
  printf ("  EE-G80:      %s\n", out);
  CHECK_EQ (st, EFI_SUCCESS);
  CHECK_EQ (gpio_log_n, 1);
  CHECK_EQ (gpio_log[0].bank, 3); CHECK_EQ (gpio_log[0].pin, 4); CHECK_EQ (gpio_log[0].val, 1);
  CHECK_EQ (gpio_dir[3][4], GPIO_PIN_OUTPUT); CHECK_EQ (gpio_func[3][4], 0);
  CHECK_EQ (boost_calls_on + boost_calls_off, 0);
  mock_delay_us = 0; run_dtb (path, 1, 1, out, &st);          // charger in: still host (port not shared)
  CHECK_EQ (st, EFI_SUCCESS); CHECK_EQ (gpio_val[3][4], 1);

  // D-28S: one port for charging and OTG
  snprintf (path, sizeof (path), "%s/rk3326-r36s-clone-d28s.dtb", root);
  if (access (path, R_OK) != 0) { printf ("  (no D-28S DTB, skipped)\n"); return; }
  mock_delay_us = 0; run_dtb (path, 0, 0, out, &st);
  printf ("  D-28S none:  %s\n", out);
  CHECK_EQ (st, EFI_SUCCESS);
  CHECK_EQ (gpio_val[3][4], 1); CHECK_EQ (boost_calls_on, 1); CHECK_EQ (boost_calls_off, 1);   // off for the probe, on for host
  CHECK_EQ (gpio_log[0].val, 0); CHECK_EQ (gpio_log[gpio_log_n - 1].val, 1);                  // charge mode first, host last
  CHECK (mock_delay_us >= 150000 && mock_delay_us < 200000);
  mock_delay_us = 0; run_dtb (path, 1, 0, out, &st);          // DC detect pin says charger
  printf ("  D-28S pin:   %s\n", out);
  CHECK_EQ (st, EFI_NOT_READY); CHECK_EQ (gpio_val[3][4], 0); CHECK_EQ (boost_calls_on, 0); CHECK_EQ (boost_calls_off, 1);
  mock_delay_us = 0; run_dtb (path, 0, 1, out, &st);          // PMIC says charger
  printf ("  D-28S pmic:  %s\n", out);
  CHECK_EQ (st, EFI_NOT_READY); CHECK_EQ (gpio_val[3][4], 0); CHECK_EQ (boost_calls_on, 0);
}

int main (int argc, char **argv)
{
  test_fifo ();
  test_policy ();
  test_poweron ();
  if (argc > 1) {
    printf ("board device trees under %s\n", argv[1]);
    test_boards (argv[1]);
  } else {
    printf ("(no DTB dir given, board tests skipped)\n");
  }
  DONE ();
}

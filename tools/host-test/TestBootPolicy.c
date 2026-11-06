/*
  Host test of BootPolicyLib: boot.ini-android's decisions for every start
  case, against the command lines U-Boot builds from the same boot.ini.
    TestBootPolicy <boot.ini-android> <boot.ini-uefi-hook.txt or "">
*/

#include <Base.h>
#include <Library/BootPolicyLib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int  mPass, mFail;

void  UbootSerialFromCpuId (const unsigned char *cpuid, char *out);   // UbootSerialRef.c

#define CHECK(c, ...)                                         \
  do {                                                        \
    if (c) { mPass++; } else {                                \
      mFail++; printf ("FAIL %s:%d: ", __FILE__, __LINE__);   \
      printf (__VA_ARGS__); printf ("\n");                    \
    }                                                         \
  } while (0)

#define CHECK_STR(got, want, what) \
  CHECK (strcmp ((got), (want)) == 0, "%s\n  got  '%s'\n  want '%s'", (what), (got), (want))

static BOOT_TEXT
Text (const char *s)
{
  BOOT_TEXT  t = { s, s ? strlen (s) : 0 };

  return t;
}

static char *
ReadFile (const char *path, size_t *size)
{
  FILE  *f = fopen (path, "rb");
  char  *b;
  long  n;

  if (f == NULL) {
    return NULL;
  }

  fseek (f, 0, SEEK_END);
  n = ftell (f);
  fseek (f, 0, SEEK_SET);
  b = malloc (n + 1);
  if (fread (b, 1, n, f) != (size_t)n) {
    n = 0;
  }

  b[n] = 0;
  fclose (f);
  *size = n;
  return b;
}

// What boot.ini makes of android2 for a device profile (mkimg.sh device_profile).
static char *
Profile (const char *ini, const char *extra)
{
  const char  *key = "\nsetenv android2 \"";
  const char  *at  = strstr (ini, key);
  char        *out;
  size_t      head;

  if (at == NULL) {
    return NULL;
  }

  head = (at - ini) + strlen (key);
  out  = malloc (strlen (ini) + strlen (extra) + 2);
  memcpy (out, ini, head);
  sprintf (out + head, "%s %s", extra, ini + head);
  return out;
}

#define SD_BASE \
  "fw_devlink=permissive rootwait consoleblank=0 swiotlb=1 coherent_pool=1m " \
  "firmware_class.path=/vendor/firmware androidboot.hardware=rk30board androidboot.selinux=permissive androidboot.verifiedbootstate=green "
#define SD_TAIL \
  "androidboot.boot_devices=ff370000.dwmmc " \
  "console=null loglevel=4 printk.time=1 vt.global_cursor_default=0 fiq_debugger.disable=1"
#define EMMC_TAIL \
  "androidboot.boot_devices=ff390000.dwmmc " \
  "console=null loglevel=4 printk.time=1 vt.global_cursor_default=0 fiq_debugger.disable=1"
#define REV "androidboot.revision=rg351mp androidboot.serialno=8f1c2a09d3b4e5f6 "
#define BOOTLOG "console=tty0 loglevel=8 ignore_loglevel printk.devkmsg=on panic=0 androidboot.init_fatal_panic=true"

static void
Decide (
  const BOOT_FACTS  *facts,
  BOOLEAN           fn,
  BOOLEAN           sel,
  BOOLEAN           start,
  const BOOT_FILES  *files,
  BOOT_CHOICE       choice,
  BOOT_PLAN         *plan
  )
{
  BOOT_KEYS  keys = { fn, sel, start };

  CHECK (BootPolicyDecide (facts, &keys, files, choice, plan) == RETURN_SUCCESS, "BootPolicyDecide");
}

int
main (int argc, char **argv)
{
  BOOT_FACTS  f, f0;
  BOOT_FILES  files, nofiles;
  BOOT_PLAN   p;
  char        v[BOOT_VALUE_MAX];
  size_t      n, hn;
  char        *ini, *hook, *withhook, *prof;
  const char  *kt = "# Kernel Tweaks\r\nkt_args=console=ttyFIQ0 loglevel=7\r\nother=1\r\n";

  if (argc < 2) {
    fprintf (stderr, "usage: %s boot.ini-android [hook]\n", argv[0]);
    return 2;
  }

  ini = ReadFile (argv[1], &n);
  CHECK (ini != NULL, "read %s", argv[1]);
  if (ini == NULL) {
    return 1;
  }

  // U-Boot's facts from the chainload hook's command line
  memset (&f, 0, sizeof (f));
  CHECK (BootPolicyParseUbootArgs ("andr36oid.hwrev=rg351mp andr36oid.serialno=8f1c2a09d3b4e5f6 andr36oid.ram_mb=1024 andr36oid.bootpart=1:1", &f), "facts found");
  CHECK_STR (f.HwRev, "rg351mp", "hwrev");
  CHECK_STR (f.SerialNo, "8f1c2a09d3b4e5f6", "serial");
  CHECK (f.RamMb == 1024 && !f.BootFromEmmc && f.FromUboot, "ram/bootpart");
  memset (&f0, 0, sizeof (f0));
  f0.RamMb = 512;
  CHECK (BootPolicyParseUbootArgs ("console=null andr36oid.ram_mb= andr36oid.bootpart=0:1", &f0), "empty ram_mb");
  CHECK (f0.RamMb == 512 && f0.BootFromEmmc, "empty ram_mb keeps the old value, 0:1 is eMMC");
  memset (&f0, 0, sizeof (f0));
  CHECK (!BootPolicyParseUbootArgs ("root=/dev/sda andr36oid.hwrevx", &f0) && !f0.FromUboot, "no facts");

  // boot.ini parsing
  memset (&files, 0, sizeof (files));
  files.BootIni = Text (ini);
  files.BootIni.Size = n;
  CHECK (BootPolicyIniGet (&files.BootIni, "android2", &f, v, sizeof (v)), "android2 found");
  CHECK_STR (v, "androidboot.revision=rg351mp androidboot.serialno=8f1c2a09d3b4e5f6", "android2 expanded");
  CHECK (BootPolicyIniGet (&files.BootIni, "bootpart", &f, v, sizeof (v)), "bootpart found");
  CHECK_STR (v, "1:1", "bootpart: the one-line if's setenv does not count");
  CHECK (BootPolicyIniGet (&files.BootIni, "bootrootfs", &f, v, sizeof (v)), "bootrootfs");
  CHECK_STR (v, "fw_devlink=permissive rootwait consoleblank=0 swiotlb=1 coherent_pool=1m", "bootrootfs: not the low-RAM one inside if");
  CHECK (!BootPolicyIniGet (&files.BootIni, "kt_args", &f, v, sizeof (v)), "kt_args only inside if");
  CHECK (BootPolicyIniGet (&files.BootIni, "fdt_addr_r", &f, v, sizeof (v)), "fdt_addr_r");
  CHECK_STR (v, "0x01f00000", "fdt_addr_r last one");

  // A normal start from the SD card: what U-Boot builds (device report)
  Decide (&f, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK (p.Target == BootTargetAndroid && !p.Safe && !p.BootLog && !p.ShowMenu && !p.LowRam && !p.EmmcTest, "plain start");
  CHECK_STR (p.Ramdisk, "ramdisk.uimg", "ramdisk");
  CHECK_STR (p.Kernel, "Image", "kernel");
  CHECK_STR (p.Dtb, "rk3326-r36s-android.dtb", "dtb");
  CHECK_STR (p.Bootargs, SD_BASE REV SD_TAIL, "SD start");

  // Kernel Tweaks: CR LF, comments, other names ignored; last
  files.KernelTweaks = Text (kt);
  Decide (&f, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK_STR (p.Bootargs, SD_BASE REV SD_TAIL " console=ttyFIQ0 loglevel=7", "SD start with kt_args");

  // the hook appended (setenv bootargs inside if must not count)
  if ((argc > 2) && (argv[2][0] != '\0')) {
    hook = ReadFile (argv[2], &hn);
    CHECK (hook != NULL, "read hook");
    withhook = malloc (n + hn + 1);
    memcpy (withhook, ini, n);
    memcpy (withhook + n, hook, hn + 1);
    files.BootIni = Text (withhook);
    Decide (&f, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
    CHECK_STR (p.Bootargs, SD_BASE REV SD_TAIL " console=ttyFIQ0 loglevel=7", "boot.ini with the UEFI hook");
    files.BootIni = Text (ini);
  }

  // A device profile's boot.ini (R46H)
  prof = Profile (ini, "androidboot.lcd_density=304");
  CHECK (prof != NULL, "profile");
  files.BootIni = Text (prof);
  Decide (&f, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK_STR (p.Bootargs, SD_BASE "androidboot.lcd_density=304 " REV SD_TAIL " console=ttyFIQ0 loglevel=7", "R46H profile");
  files.BootIni = Text (ini);

  // No boot.ini at all: boot.ini-android's values built in
  nofiles = files;
  nofiles.BootIni = Text (NULL);
  Decide (&f, FALSE, FALSE, FALSE, &nofiles, BootChoiceNone, &p);
  CHECK_STR (p.Bootargs, SD_BASE REV SD_TAIL " console=ttyFIQ0 loglevel=7", "built-in defaults");

  // Recovery: FN on an R36S-family board
  Decide (&f, TRUE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK (p.Target == BootTargetRecovery, "FN: recovery");
  CHECK_STR (p.Ramdisk, "ramdisk-recovery.uimg", "recovery ramdisk");
  CHECK_STR (p.Bootargs, SD_BASE REV SD_TAIL " dw_mmc_rockchip.builtin_emmc=1", "recovery from SD: no kt_args, eMMC visible");

  // FN on a board boot.ini reads no keys on (Soy Sauce, unknown)
  f0 = f;
  strcpy (f0.HwRev, "v00");
  Decide (&f0, TRUE, TRUE, TRUE, &files, BootChoiceNone, &p);
  CHECK (p.Target == BootTargetAndroid && !p.Safe && !p.ShowMenu, "keys ignored on v00");

  // recovery.txt works everywhere
  files.RecoveryTxt = TRUE;
  Decide (&f0, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK (p.Target == BootTargetRecovery, "recovery.txt");
  // ... but the menu's choice wins
  Decide (&f0, FALSE, FALSE, FALSE, &files, BootChoiceAndroid, &p);
  CHECK (p.Target == BootTargetAndroid, "menu Android over recovery.txt");
  files.RecoveryTxt = FALSE;

  // Select: the menu (boot.ini: boot log); the menu's boot log
  Decide (&f, FALSE, TRUE, FALSE, &files, BootChoiceNone, &p);
  CHECK (p.ShowMenu && !p.Safe && p.Target == BootTargetAndroid, "Select: menu");
  Decide (&f, FALSE, FALSE, FALSE, &files, BootChoiceBootLog, &p);
  CHECK_STR (p.Bootargs, SD_BASE REV SD_TAIL " console=ttyFIQ0 loglevel=7 " BOOTLOG, "boot log");

  // Start+Select: safe start
  Decide (&f, FALSE, TRUE, TRUE, &files, BootChoiceNone, &p);
  CHECK (p.Safe && !p.ShowMenu && !p.BootLog, "Start+Select: safe");
  CHECK_STR (p.Ramdisk, "ramdisk-stock.uimg", "safe ramdisk");
  CHECK_STR (p.Bootargs, SD_BASE REV SD_TAIL " androidboot.andr36oid_safe=1", "safe: no kt_args");
  Decide (&f, FALSE, FALSE, FALSE, &files, BootChoiceSafe, &p);
  CHECK_STR (p.Bootargs, SD_BASE REV SD_TAIL " androidboot.andr36oid_safe=1", "menu safe start");
  Decide (&f, FALSE, FALSE, FALSE, &files, BootChoiceRecovery, &p);
  CHECK (p.Target == BootTargetRecovery, "menu recovery");

  // Low RAM
  f0 = f;
  f0.RamMb = 512;
  Decide (&f0, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK (p.LowRam, "512 MB: low");
  CHECK_STR (p.Bootargs, "cma=64M " SD_BASE REV SD_TAIL " console=ttyFIQ0 loglevel=7", "cma=64M first");
  f0.RamMb = 0;
  Decide (&f0, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK (!p.LowRam, "unknown RAM: no warning");

  // eMMC install, without and with a data card
  f0 = f;
  f0.BootFromEmmc = TRUE;
  Decide (&f0, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK_STR (p.Bootargs, SD_BASE REV EMMC_TAIL " console=ttyFIQ0 loglevel=7 dw_mmc_rockchip.builtin_emmc=system androidboot.fstab_suffix=emmc", "eMMC start");
  files.Datacard = Text ("datacard=3f2a9b10-7c4d-4e8a-9b21-0d5c6e7f8a90\r\n");
  Decide (&f0, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK_STR (p.Bootargs, SD_BASE REV EMMC_TAIL " console=ttyFIQ0 loglevel=7 dw_mmc_rockchip.builtin_emmc=system androidboot.fstab_suffix=emmcsd androidboot.datacard=3f2a9b10-7c4d-4e8a-9b21-0d5c6e7f8a90", "eMMC + data card");
  Decide (&f0, TRUE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK_STR (p.Bootargs, SD_BASE REV EMMC_TAIL " dw_mmc_rockchip.builtin_emmc=system androidboot.fstab_suffix=emmcsd androidboot.datacard=3f2a9b10-7c4d-4e8a-9b21-0d5c6e7f8a90", "eMMC recovery");
  files.Datacard = Text (NULL);

  // eMMC test
  files.EmmcTest = Text ("emmc_test=start\n");
  Decide (&f, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK (p.EmmcTest, "emmc_test=start");
  Decide (&f, FALSE, TRUE, FALSE, &files, BootChoiceNone, &p);
  CHECK (!p.EmmcTest, "Select leaves the eMMC test");
  Decide (&f0, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK (!p.EmmcTest, "not from the eMMC");
  files.EmmcTest = Text ("emmc_test=tried\n");
  Decide (&f, FALSE, FALSE, FALSE, &files, BootChoiceNone, &p);
  CHECK (!p.EmmcTest, "tried");
  files.EmmcTest = Text (NULL);

  // env text corner cases
  {
    BOOT_TEXT  t = Text ("a=1\nkt_args=\nb=2");

    CHECK (!BootPolicyEnvGet (&t, "kt_args", v, sizeof (v)), "empty value is unset");
    CHECK (BootPolicyEnvGet (&t, "b", v, sizeof (v)) && strcmp (v, "2") == 0, "no newline at the end");
    CHECK (!BootPolicyEnvGet (&t, "c", v, sizeof (v)), "missing");
    t = Text ("kt_args=0123456789");
    CHECK (!BootPolicyEnvGet (&t, "kt_args", v, 5), "too long is unset");
  }

  // Overflow of the command line
  {
    static char  big[4096];
    BOOT_FILES   bf = files;

    memset (big, 'x', sizeof (big) - 1);
    memcpy (big, "kt_args=", 8);
    bf.KernelTweaks = Text (big);
    // a value that long is refused by EnvGet already; force the line itself long
    bf.KernelTweaks = Text (NULL);
    {
      static char  bigini[4096];

      snprintf (bigini, sizeof (bigini), "setenv console \"%.*s\"\n", BOOT_VALUE_MAX - 2, big + 8);
      bf.BootIni = Text (bigini);
    }

    Decide (&f, FALSE, FALSE, FALSE, &bf, BootChoiceNone, &p);
    CHECK (strlen (p.Bootargs) < BOOT_ARGS_MAX, "long console fits");
  }

  // Serial number from the CPU ID, against U-Boot's own code
  {
    static const UINT8  Qemu[BOOT_CPU_ID_SIZE] = "QEMU virt cpu id";   // qemuvirt BootPlatformLib
    UINT8               Id[BOOT_CPU_ID_SIZE];
    char                Got[64], Want[80];
    int                 i, j, Bad = 0, Short = 0;

    CHECK (BootPolicySerialFromCpuId (Qemu, Got, sizeof (Got)), "QEMU CPU ID");
    UbootSerialFromCpuId (Qemu, Want);
    CHECK_STR (Got, Want, "QEMU CPU ID serial");
    printf ("     QEMU CPU ID serial %s\n", Got);

    srand (36);
    for (i = 0; i < 200000; i++) {
      for (j = 0; j < BOOT_CPU_ID_SIZE; j++) {
        Id[j] = (UINT8)rand ();
      }

      if (i < 256) {          // one byte set, the rest zero: small CRCs, short serials
        memset (Id, 0, sizeof (Id));
        Id[i % BOOT_CPU_ID_SIZE] = (UINT8)(i % 255 + 1);
      }

      if (!BootPolicySerialFromCpuId (Id, Got, sizeof (Got))) {
        Bad++;
        continue;
      }

      UbootSerialFromCpuId (Id, Want);
      if (strcmp (Got, Want) != 0) {
        if (Bad++ < 5) {
          printf ("FAIL serial %d: got %s want %s\n", i, Got, Want);
        }
      }

      Short += strlen (Got) < 16;
    }

    CHECK (Bad == 0, "%d of 200000 CPU IDs give another serial than U-Boot's", Bad);
    CHECK (Short > 1000, "only %d serials shorter than 16 digits tested", Short);
    printf ("     200000 CPU IDs match U-Boot (%d with leading zeros dropped)\n", Short);

    memset (Id, 0, sizeof (Id));
    CHECK (!BootPolicySerialFromCpuId (Id, Got, sizeof (Got)) && Got[0] == 0, "all-zero OTP refused");
    memset (Id, 0xFF, sizeof (Id));
    CHECK (!BootPolicySerialFromCpuId (Id, Got, sizeof (Got)) && Got[0] == 0, "all-0xFF OTP refused");
    CHECK (!BootPolicySerialFromCpuId (Qemu, Got, 16), "16-byte buffer refused");
  }

  // Screen DTB names, U-Boot's cmd/hwrev.c
  CHECK_STR (BootPolicyScreenDtbName ("rg351mp"), "rg351mp-uboot.dtb", "rg351mp");
  CHECK_STR (BootPolicyScreenDtbName ("rg351v"), "rg351v-uboot.dtb", "rg351v");
  CHECK_STR (BootPolicyScreenDtbName ("rg351p"), "rg351p-uboot.dtb", "rg351p");
  CHECK_STR (BootPolicyScreenDtbName ("v00"), "rg351p-uboot.dtb", "v00");
  CHECK_STR (BootPolicyScreenDtbName (""), "rg351p-uboot.dtb", "empty");
  CHECK_STR (BootPolicyScreenDtbName ("rg351mpx"), "rg351p-uboot.dtb", "rg351mpx");

  printf ("TestBootPolicy: %d passed, %d failed\n", mPass, mFail);
  return mFail ? 1 : 0;
}

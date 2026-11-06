/** @file
  The decisions BOOT/boot.ini makes today (device repo BOOT/boot.ini-android),
  as a pure function: facts, keys and BOOT files in, kernel/ramdisk/DTB names
  and the kernel command line out. No UEFI calls, no library calls: the host
  tests build it with gcc.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _BOOT_POLICY_LIB_H_
#define _BOOT_POLICY_LIB_H_

#include <Base.h>

#define BOOT_ARGS_MAX     2048
#define BOOT_VALUE_MAX    512
#define BOOT_CPU_ID_SIZE  16      // PX30 OTP bytes 0x07-0x16 (U-Boot's CPUID_OFF/LEN)

typedef enum {
  BootTargetAndroid,
  BootTargetRecovery
} BOOT_TARGET;

//
// What the boot menu picked; BootChoiceNone for a normal start.
//
typedef enum {
  BootChoiceNone,
  BootChoiceAndroid,
  BootChoiceBootLog,      // Android with the kernel log on the screen
  BootChoiceSafe,         // Android without Kernel Tweaks and Magisk
  BootChoiceRecovery
} BOOT_CHOICE;

typedef struct {
  CHAR8      HwRev[16];        // U-Boot's hwrev: rg351mp, rg351v, rg351p or v00
  CHAR8      SerialNo[64];     // U-Boot's serial#
  UINT32     RamMb;            // U-Boot's ram_mb, 0 = unknown
  BOOLEAN    BootFromEmmc;     // U-Boot's andr36oid_part is 0:1
  BOOLEAN    FromUboot;        // these came from U-Boot's command line
} BOOT_FACTS;

typedef struct {
  BOOLEAN    Fn;
  BOOLEAN    Select;
  BOOLEAN    Start;
} BOOT_KEYS;

typedef struct {
  CONST CHAR8    *Data;        // NULL: the file is not there
  UINTN          Size;
} BOOT_TEXT;

typedef struct {
  BOOT_TEXT    BootIni;        // boot.ini: the device profile's kernel arguments
  BOOT_TEXT    KernelTweaks;   // kernel-tweaks.txt: kt_args=...
  BOOT_TEXT    Datacard;       // datacard.txt on an eMMC install: datacard=<GUID>
  BOOT_TEXT    EmmcTest;       // emmc-test.txt: emmc_test=start|tried|ok
  BOOLEAN      RecoveryTxt;    // recovery.txt or recovery.txt.txt exists
} BOOT_FILES;

typedef struct {
  BOOT_TARGET    Target;
  BOOLEAN        Safe;         // Start+Select: no Kernel Tweaks, stock ramdisk
  BOOLEAN        BootLog;      // kernel log on the screen
  BOOLEAN        ShowMenu;     // Select held: the boot menu (boot.ini showed the log)
  BOOLEAN        LowRam;       // under 768 MB: cma=64M and a 10 s warning
  BOOLEAN        EmmcTest;     // emmc-test.txt says start: try the eMMC once
  CONST CHAR8    *Kernel;
  CONST CHAR8    *Ramdisk;
  CONST CHAR8    *Dtb;
  CHAR8          Bootargs[BOOT_ARGS_MAX];
} BOOT_PLAN;

//
// File names on BOOT, as boot.ini uses them.
//
#define BOOT_FILE_KERNEL           "Image"
#define BOOT_FILE_DTB              "rk3326-r36s-android.dtb"
#define BOOT_FILE_RAMDISK          "ramdisk.uimg"
#define BOOT_FILE_RAMDISK_STOCK    "ramdisk-stock.uimg"
#define BOOT_FILE_RAMDISK_RECOVERY "ramdisk-recovery.uimg"
#define BOOT_FILE_BOOT_INI         "boot.ini"
#define BOOT_FILE_KERNEL_TWEAKS    "kernel-tweaks.txt"
#define BOOT_FILE_DATACARD         "datacard.txt"
#define BOOT_FILE_EMMC_TEST        "emmc-test.txt"
#define BOOT_FILE_RECOVERY         "recovery.txt"
#define BOOT_FILE_RECOVERY_WIN     "recovery.txt.txt"

/**
  Take U-Boot's facts from the command line the chainload hook gives us
  (andr36oid.hwrev= andr36oid.serialno= andr36oid.ram_mb= andr36oid.bootpart=).
  Keys not there leave Facts as they were. Returns TRUE if any was there.
**/
BOOLEAN
EFIAPI
BootPolicyParseUbootArgs (
  IN     CONST CHAR8  *Bootargs,
  IN OUT BOOT_FACTS   *Facts
  );

/**
  One variable from a U-Boot "env import -t" text (name=value lines, #
  comments, CR LF or LF). Returns FALSE if it is not set (or empty).
**/
BOOLEAN
EFIAPI
BootPolicyEnvGet (
  IN  CONST BOOT_TEXT  *Text,
  IN  CONST CHAR8      *Name,
  OUT CHAR8            *Value,
  IN  UINTN            ValueSize
  );

/**
  A boot.ini "setenv Name ..." outside any if/fi, with ${hwrev} and
  ${serial#} filled in from Facts and other ${...} dropped. FALSE if there
  is no such line.
**/
BOOLEAN
EFIAPI
BootPolicyIniGet (
  IN  CONST BOOT_TEXT   *BootIni,
  IN  CONST CHAR8       *Name,
  IN  CONST BOOT_FACTS  *Facts,
  OUT CHAR8             *Value,
  IN  UINTN             ValueSize
  );

/**
  Everything boot.ini decides. Choice overrides the keys (boot menu).
**/
RETURN_STATUS
EFIAPI
BootPolicyDecide (
  IN  CONST BOOT_FACTS  *Facts,
  IN  CONST BOOT_KEYS   *Keys,
  IN  CONST BOOT_FILES  *Files,
  IN  BOOT_CHOICE       Choice,
  OUT BOOT_PLAN         *Plan
  );

/**
  The serial number U-Boot's rockchip_set_serialno (arch/arm/mach-rockchip/
  board.c:96-153) makes from a CPU ID: CRC32 without complement of the odd
  bytes, then of the even bytes seeded with the first CRC, as one 64-bit
  number in lower-case hex without leading zeros ("%llx"). FALSE (and no
  serial) for a CPU ID of all 0x00 or all 0xFF, an OTP that was not read.
**/
BOOLEAN
EFIAPI
BootPolicySerialFromCpuId (
  IN  CONST UINT8  *CpuId,
  OUT CHAR8        *Serial,
  IN  UINTN        SerialSize
  );

/**
  The screen DTB U-Boot loads for this hwrev (cmd/hwrev.c): rg351mp-uboot.dtb,
  rg351v-uboot.dtb, else (rg351p, v00) rg351p-uboot.dtb.
**/
CONST CHAR8 *
EFIAPI
BootPolicyScreenDtbName (
  IN CONST CHAR8  *HwRev
  );

/**
  TRUE if the device path runs through a USB device (a USB() node, which
  UsbBusDxe puts in every device's path): a USB stick, not the SD card or
  the eMMC. A node shorter than its header ends the walk, as do 64 nodes.
**/
BOOLEAN
EFIAPI
BootPolicyIsUsbPath (
  IN CONST VOID  *DevicePath OPTIONAL
  );

#endif

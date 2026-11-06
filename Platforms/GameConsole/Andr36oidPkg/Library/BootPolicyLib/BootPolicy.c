/** @file
  BOOT/boot.ini's decisions (device repo BOOT/boot.ini-android) without
  U-Boot: which kernel, ramdisk and DTB, and the kernel command line.
  Pure C on purpose (no library calls), the host tests build it with gcc.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BootPolicyLib.h>

//
// boot.ini-android's values, used when BOOT/boot.ini is missing or has no
// such setenv. A device profile's boot.ini puts its extra arguments at the
// start of android2.
//
STATIC CONST CHAR8  mDefBootrootfs[] = "fw_devlink=permissive rootwait consoleblank=0 swiotlb=1 coherent_pool=1m";
STATIC CONST CHAR8  mDefConsole[]    = "console=null loglevel=4 printk.time=1 vt.global_cursor_default=0 fiq_debugger.disable=1";
STATIC CONST CHAR8  mDefAndroid[]    = "firmware_class.path=/vendor/firmware androidboot.hardware=rk30board androidboot.selinux=permissive androidboot.verifiedbootstate=green";
STATIC CONST CHAR8  mDefAndroid2[]   = "androidboot.revision=${hwrev} androidboot.serialno=${serial#}";

STATIC CONST CHAR8  mBootLogArgs[]   = "console=tty0 loglevel=8 ignore_loglevel printk.devkmsg=on panic=0 androidboot.init_fatal_panic=true";
STATIC CONST CHAR8  mSafeArgs[]      = "androidboot.andr36oid_safe=1";
STATIC CONST CHAR8  mLowRamArgs[]    = "cma=64M";
STATIC CONST CHAR8  mSdBootDevs[]    = "androidboot.boot_devices=ff370000.dwmmc";
STATIC CONST CHAR8  mEmmcBootDevs[]  = "androidboot.boot_devices=ff390000.dwmmc";
STATIC CONST CHAR8  mEmmcArgs[]      = "dw_mmc_rockchip.builtin_emmc=system androidboot.fstab_suffix=emmc";
STATIC CONST CHAR8  mEmmcSdArgs[]    = "dw_mmc_rockchip.builtin_emmc=system androidboot.fstab_suffix=emmcsd androidboot.datacard=";
STATIC CONST CHAR8  mRecoveryEmmc[]  = "dw_mmc_rockchip.builtin_emmc=1";

#define LOW_RAM_MB  768

STATIC
UINTN
Len (
  IN CONST CHAR8  *S
  )
{
  UINTN  N = 0;

  while (S[N] != '\0') {
    N++;
  }

  return N;
}

STATIC
BOOLEAN
Same (
  IN CONST CHAR8  *A,
  IN UINTN        ALen,
  IN CONST CHAR8  *B
  )
{
  UINTN  I;

  for (I = 0; I < ALen; I++) {
    if (A[I] != B[I]) {
      return FALSE;
    }
  }

  return B[ALen] == '\0';
}

STATIC
BOOLEAN
IsSpace (
  IN CHAR8  C
  )
{
  return C == ' ' || C == '\t' || C == '\r' || C == '\n';
}

//
// Bounded string builder: an overflow sticks and makes the result invalid.
//
typedef struct {
  CHAR8      *Buf;
  UINTN      Size;
  UINTN      Pos;
  BOOLEAN    Overflow;
} OUT_BUF;

STATIC
VOID
OutInit (
  OUT OUT_BUF  *O,
  IN  CHAR8    *Buf,
  IN  UINTN    Size
  )
{
  O->Buf      = Buf;
  O->Size     = Size;
  O->Pos      = 0;
  O->Overflow = (Size == 0);
  if (Size != 0) {
    Buf[0] = '\0';
  }
}

STATIC
VOID
OutChars (
  IN OUT OUT_BUF   *O,
  IN CONST CHAR8   *S,
  IN UINTN         N
  )
{
  UINTN  I;

  for (I = 0; I < N; I++) {
    if (O->Pos + 1 >= O->Size) {
      O->Overflow = TRUE;
      break;
    }

    O->Buf[O->Pos++] = S[I];
  }

  if (O->Size != 0) {
    O->Buf[O->Pos] = '\0';
  }
}

//
// One kernel argument group: skipped when empty, one space between groups,
// runs of blanks squeezed (U-Boot left two spaces around an empty variable).
//
STATIC
VOID
OutArgs (
  IN OUT OUT_BUF   *O,
  IN CONST CHAR8   *S
  )
{
  UINTN  I = 0;
  UINTN  Start;

  while (S[I] != '\0') {
    while (IsSpace (S[I])) {
      I++;
    }

    Start = I;
    while (S[I] != '\0' && !IsSpace (S[I])) {
      I++;
    }

    if (I > Start) {
      if (O->Pos != 0) {
        OutChars (O, " ", 1);
      }

      OutChars (O, S + Start, I - Start);
    }
  }
}

BOOLEAN
EFIAPI
BootPolicyParseUbootArgs (
  IN     CONST CHAR8  *Bootargs,
  IN OUT BOOT_FACTS   *Facts
  )
{
  STATIC CONST CHAR8  Hw[]   = "andr36oid.hwrev=";
  STATIC CONST CHAR8  Sn[]   = "andr36oid.serialno=";
  STATIC CONST CHAR8  Ram[]  = "andr36oid.ram_mb=";
  STATIC CONST CHAR8  Part[] = "andr36oid.bootpart=";
  CONST CHAR8         *P;
  CONST CHAR8         *Word;
  CONST CHAR8         *V;
  UINTN               WLen;
  UINTN               VLen;
  UINTN               I;
  UINT32              Mb;
  BOOLEAN             Found = FALSE;
  OUT_BUF             O;

  if (Bootargs == NULL) {
    return FALSE;
  }

  for (P = Bootargs; *P != '\0';) {
    while (IsSpace (*P)) {
      P++;
    }

    Word = P;
    while (*P != '\0' && !IsSpace (*P)) {
      P++;
    }

    WLen = (UINTN)(P - Word);
    if (WLen == 0) {
      continue;
    }

    if ((WLen >= sizeof (Hw) - 1) && Same (Word, sizeof (Hw) - 1, Hw)) {
      V    = Word + sizeof (Hw) - 1;
      VLen = WLen - (sizeof (Hw) - 1);
      OutInit (&O, Facts->HwRev, sizeof (Facts->HwRev));
      OutChars (&O, V, VLen);
      if (O.Overflow) {
        Facts->HwRev[0] = '\0';
      }

      Found = TRUE;
    } else if ((WLen >= sizeof (Sn) - 1) && Same (Word, sizeof (Sn) - 1, Sn)) {
      V    = Word + sizeof (Sn) - 1;
      VLen = WLen - (sizeof (Sn) - 1);
      OutInit (&O, Facts->SerialNo, sizeof (Facts->SerialNo));
      OutChars (&O, V, VLen);
      if (O.Overflow) {
        Facts->SerialNo[0] = '\0';
      }

      Found = TRUE;
    } else if ((WLen >= sizeof (Ram) - 1) && Same (Word, sizeof (Ram) - 1, Ram)) {
      V    = Word + sizeof (Ram) - 1;
      VLen = WLen - (sizeof (Ram) - 1);
      Mb   = 0;
      for (I = 0; I < VLen && I < 7; I++) {
        if ((V[I] < '0') || (V[I] > '9')) {
          Mb = 0;
          break;
        }

        Mb = Mb * 10 + (UINT32)(V[I] - '0');
      }

      // Older U-Boots leave ram_mb empty: unknown, keep what we had.
      if (Mb != 0) {
        Facts->RamMb = Mb;
      }

      Found = TRUE;
    } else if ((WLen >= sizeof (Part) - 1) && Same (Word, sizeof (Part) - 1, Part)) {
      V                   = Word + sizeof (Part) - 1;
      VLen                = WLen - (sizeof (Part) - 1);
      Facts->BootFromEmmc = (VLen == 3) && Same (V, 3, "0:1");
      Found               = TRUE;
    }
  }

  if (Found) {
    Facts->FromUboot = TRUE;
  }

  return Found;
}

BOOLEAN
EFIAPI
BootPolicyEnvGet (
  IN  CONST BOOT_TEXT  *Text,
  IN  CONST CHAR8      *Name,
  OUT CHAR8            *Value,
  IN  UINTN            ValueSize
  )
{
  UINTN       NameLen = Len (Name);
  UINTN       I       = 0;
  UINTN       Line;
  UINTN       End;
  BOOLEAN     Got = FALSE;
  OUT_BUF     O;
  CONST CHAR8 *D;

  OutInit (&O, Value, ValueSize);
  if ((Text == NULL) || (Text->Data == NULL)) {
    return FALSE;
  }

  D = Text->Data;
  while (I < Text->Size) {
    Line = I;
    while (I < Text->Size && D[I] != '\n' && D[I] != '\0') {
      I++;
    }

    End = I;
    I++;
    while (End > Line && (D[End - 1] == '\r')) {
      End--;
    }

    if ((End - Line > NameLen) && (D[Line + NameLen] == '=') && Same (D + Line, NameLen, Name)) {
      // env import: the last one wins
      OutInit (&O, Value, ValueSize);
      OutChars (&O, D + Line + NameLen + 1, End - Line - NameLen - 1);
      Got = !O.Overflow && (O.Pos != 0);
    }
  }

  if (!Got && (ValueSize != 0)) {
    Value[0] = '\0';
  }

  return Got;
}

//
// ${hwrev} and ${serial#} as U-Boot has them; any other variable is empty
// (the top-level setenvs boot.ini-android's command line is made of use
// only these two).
//
STATIC
VOID
OutExpanded (
  IN OUT OUT_BUF          *O,
  IN CONST CHAR8          *S,
  IN UINTN                N,
  IN CONST BOOT_FACTS     *Facts
  )
{
  UINTN  I = 0;
  UINTN  Start;

  while (I < N) {
    if ((S[I] == '$') && (I + 1 < N) && (S[I + 1] == '{')) {
      Start = I + 2;
      for (I = Start; I < N && S[I] != '}'; I++) {
      }

      if (Same (S + Start, I - Start, "hwrev")) {
        OutChars (O, Facts->HwRev, Len (Facts->HwRev));
      } else if (Same (S + Start, I - Start, "serial#")) {
        OutChars (O, Facts->SerialNo, Len (Facts->SerialNo));
      }

      I++;
      continue;
    }

    OutChars (O, S + I, 1);
    I++;
  }
}

//
// Split boot.ini into hush statements (';' or a line end outside quotes,
// '#' at a word start begins a comment) and follow if/fi depth.
//
BOOLEAN
EFIAPI
BootPolicyIniGet (
  IN  CONST BOOT_TEXT   *BootIni,
  IN  CONST CHAR8       *Name,
  IN  CONST BOOT_FACTS  *Facts,
  OUT CHAR8             *Value,
  IN  UINTN             ValueSize
  )
{
  CONST CHAR8  *D;
  UINTN        Size;
  UINTN        I = 0;
  UINTN        Start;
  UINTN        End;
  UINTN        W;
  UINTN        WEnd;
  INTN         Depth = 0;
  CHAR8        Quote;
  BOOLEAN      Got = FALSE;
  OUT_BUF      O;

  OutInit (&O, Value, ValueSize);
  if ((BootIni == NULL) || (BootIni->Data == NULL)) {
    return FALSE;
  }

  D    = BootIni->Data;
  Size = BootIni->Size;
  while (I < Size) {
    // One statement: [Start, End)
    Start = I;
    Quote = 0;
    while (I < Size && D[I] != '\0') {
      if (Quote != 0) {
        if (D[I] == Quote) {
          Quote = 0;
        }
      } else if ((D[I] == '"') || (D[I] == '\'')) {
        Quote = D[I];
      } else if ((D[I] == ';') || (D[I] == '\n')) {
        break;
      } else if ((D[I] == '#') && ((I == Start) || IsSpace (D[I - 1]))) {
        // comment to the end of the line
        End = I;
        while (I < Size && D[I] != '\n' && D[I] != '\0') {
          I++;
        }

        goto Statement;
      }

      I++;
    }

    End = I;
Statement:
    if ((I < Size) && (D[I] == '\0')) {
      I = Size;
    } else {
      I++;
    }

    // Keywords that only open a block body
    W = Start;
    for ( ; ;) {
      while (W < End && IsSpace (D[W])) {
        W++;
      }

      for (WEnd = W; WEnd < End && !IsSpace (D[WEnd]); WEnd++) {
      }

      if (Same (D + W, WEnd - W, "then") || Same (D + W, WEnd - W, "else") || Same (D + W, WEnd - W, "do")) {
        W = WEnd;
        continue;
      }

      break;
    }

    if (Same (D + W, WEnd - W, "if") || Same (D + W, WEnd - W, "while") ||
        Same (D + W, WEnd - W, "until") || Same (D + W, WEnd - W, "for"))
    {
      Depth++;
      continue;
    }

    if (Same (D + W, WEnd - W, "fi") || Same (D + W, WEnd - W, "done")) {
      if (Depth > 0) {
        Depth--;
      }

      continue;
    }

    if ((Depth != 0) || !Same (D + W, WEnd - W, "setenv")) {
      continue;
    }

    // setenv <name> <value...>
    for (W = WEnd; W < End && IsSpace (D[W]); W++) {
    }

    for (WEnd = W; WEnd < End && !IsSpace (D[WEnd]); WEnd++) {
    }

    if (!Same (D + W, WEnd - W, Name)) {
      continue;
    }

    for (W = WEnd; W < End && IsSpace (D[W]); W++) {
    }

    while (End > W && IsSpace (D[End - 1])) {
      End--;
    }

    // The last top-level setenv wins, as when the script runs.
    OutInit (&O, Value, ValueSize);
    if ((End - W >= 2) && ((D[W] == '"') || (D[W] == '\'')) && (D[End - 1] == D[W])) {
      OutExpanded (&O, D + W + 1, End - W - 2, Facts);
    } else {
      OutExpanded (&O, D + W, End - W, Facts);
    }

    Got = !O.Overflow;
  }

  if (!Got && (ValueSize != 0)) {
    Value[0] = '\0';
  }

  return Got;
}

STATIC
BOOLEAN
HasButtons (
  IN CONST BOOT_FACTS  *Facts
  )
{
  // boot.ini reads FN, Select and Start only on these two families.
  return Same (Facts->HwRev, Len (Facts->HwRev), "rg351mp") || Same (Facts->HwRev, Len (Facts->HwRev), "rg351v");
}

RETURN_STATUS
EFIAPI
BootPolicyDecide (
  IN  CONST BOOT_FACTS  *Facts,
  IN  CONST BOOT_KEYS   *Keys,
  IN  CONST BOOT_FILES  *Files,
  IN  BOOT_CHOICE       Choice,
  OUT BOOT_PLAN         *Plan
  )
{
  CHAR8    Value[BOOT_VALUE_MAX];
  CHAR8    Datacard[80];
  OUT_BUF  O;
  BOOLEAN  Fn;
  BOOLEAN  Select;
  BOOLEAN  Start;
  UINTN    I;

  for (I = 0; I < sizeof (*Plan); I++) {
    ((UINT8 *)Plan)[I] = 0;
  }

  Fn     = HasButtons (Facts) && Keys->Fn;
  Select = HasButtons (Facts) && Keys->Select;
  Start  = HasButtons (Facts) && Keys->Start;

  Plan->Target = BootTargetAndroid;
  switch (Choice) {
    case BootChoiceNone:
      if (Select && Start) {
        Plan->Safe = TRUE;
      } else if (Select) {
        // boot.ini turned the boot log on; the menu offers it
        Plan->ShowMenu = TRUE;
      }

      if (Fn || Files->RecoveryTxt) {
        Plan->Target = BootTargetRecovery;
      }

      // emmc-test.txt from the SD card's BOOT; Select (+Start) leaves it for later
      if (!Facts->BootFromEmmc && !Select &&
          BootPolicyEnvGet (&Files->EmmcTest, "emmc_test", Value, sizeof (Value)) && Same (Value, Len (Value), "start"))
      {
        Plan->EmmcTest = TRUE;
      }

      break;
    case BootChoiceBootLog:
      Plan->BootLog = TRUE;
      break;
    case BootChoiceSafe:
      Plan->Safe = TRUE;
      break;
    case BootChoiceRecovery:
      Plan->Target = BootTargetRecovery;
      break;
    default:
      break;
  }

  Plan->LowRam = (Facts->RamMb != 0) && (Facts->RamMb < LOW_RAM_MB);
  Plan->Kernel = BOOT_FILE_KERNEL;
  Plan->Dtb    = BOOT_FILE_DTB;
  if (Plan->Target == BootTargetRecovery) {
    Plan->Ramdisk = BOOT_FILE_RAMDISK_RECOVERY;
  } else {
    Plan->Ramdisk = Plan->Safe ? BOOT_FILE_RAMDISK_STOCK : BOOT_FILE_RAMDISK;
  }

  //
  // "${bootrootfs} ${android} ${android2} ${bootdevs} ${console} [${kt_args}]
  //  ${emmc_args} ${bootlog_args} ${safe_args}", kt_args only for Android.
  //
  OutInit (&O, Plan->Bootargs, sizeof (Plan->Bootargs));
  if (Plan->LowRam) {
    OutArgs (&O, mLowRamArgs);
  }

  OutArgs (&O, BootPolicyIniGet (&Files->BootIni, "bootrootfs", Facts, Value, sizeof (Value)) ? Value : mDefBootrootfs);
  OutArgs (&O, BootPolicyIniGet (&Files->BootIni, "android", Facts, Value, sizeof (Value)) ? Value : mDefAndroid);
  if (!BootPolicyIniGet (&Files->BootIni, "android2", Facts, Value, sizeof (Value))) {
    OUT_BUF  E;

    OutInit (&E, Value, sizeof (Value));
    OutExpanded (&E, mDefAndroid2, sizeof (mDefAndroid2) - 1, Facts);
  }

  OutArgs (&O, Value);
  OutArgs (&O, Facts->BootFromEmmc ? mEmmcBootDevs : mSdBootDevs);
  OutArgs (&O, BootPolicyIniGet (&Files->BootIni, "console", Facts, Value, sizeof (Value)) ? Value : mDefConsole);

  // Kernel Tweaks: Android only, not in a safe start; after the defaults so they win
  if ((Plan->Target == BootTargetAndroid) && !Plan->Safe &&
      BootPolicyEnvGet (&Files->KernelTweaks, "kt_args", Value, sizeof (Value)))
  {
    OutArgs (&O, Value);
  }

  // emmc_args after the tweaks: kernel-tweaks.txt can't switch an eMMC install's eMMC off
  if (Facts->BootFromEmmc) {
    if (BootPolicyEnvGet (&Files->Datacard, "datacard", Datacard, sizeof (Datacard))) {
      OutArgs (&O, mEmmcSdArgs);
      OutChars (&O, Datacard, Len (Datacard));
    } else {
      OutArgs (&O, mEmmcArgs);
    }
  } else if (Plan->Target == BootTargetRecovery) {
    OutArgs (&O, mRecoveryEmmc);
  }

  if (Plan->BootLog) {
    OutArgs (&O, mBootLogArgs);
  }

  if (Plan->Safe) {
    OutArgs (&O, mSafeArgs);
  }

  return O.Overflow ? RETURN_BUFFER_TOO_SMALL : RETURN_SUCCESS;
}

//
// U-Boot's crc32_no_comp (lib/crc32.c): reflected CRC-32, no pre or post
// inversion. Bitwise: eight bytes at a time don't need a table.
//
STATIC
UINT32
Crc32NoComp (
  IN UINT32       Crc,
  IN CONST UINT8  *Buf,
  IN UINTN        Size
  )
{
  UINTN  Bit;

  while (Size-- > 0) {
    Crc ^= *Buf++;
    for (Bit = 0; Bit < 8; Bit++) {
      Crc = (Crc >> 1) ^ (0xEDB88320U & (0U - (Crc & 1)));
    }
  }

  return Crc;
}

BOOLEAN
EFIAPI
BootPolicySerialFromCpuId (
  IN  CONST UINT8  *CpuId,
  OUT CHAR8        *Serial,
  IN  UINTN        SerialSize
  )
{
  UINT8   Low[BOOT_CPU_ID_SIZE / 2];
  UINT8   High[BOOT_CPU_ID_SIZE / 2];
  UINT64  Value;
  UINTN   I;
  UINTN   N;
  BOOLEAN AllZero = TRUE;
  BOOLEAN AllOnes = TRUE;

  if ((Serial == NULL) || (SerialSize < 17)) {
    return FALSE;
  }

  Serial[0] = '\0';
  for (I = 0; I < BOOT_CPU_ID_SIZE; I++) {
    AllZero = AllZero && (CpuId[I] == 0x00);
    AllOnes = AllOnes && (CpuId[I] == 0xFF);
  }

  if (AllZero || AllOnes) {
    return FALSE;
  }

  for (I = 0; I < BOOT_CPU_ID_SIZE / 2; I++) {
    Low[I]  = CpuId[1 + (I << 1)];
    High[I] = CpuId[I << 1];
  }

  Value  = Crc32NoComp (0, Low, sizeof (Low));
  Value |= (UINT64)Crc32NoComp ((UINT32)Value, High, sizeof (High)) << 32;

  // "%llx": lower-case hex, no leading zeros
  for (N = 1; (N < 16) && ((Value >> (4 * N)) != 0); N++) {
  }

  for (I = 0; I < N; I++) {
    Serial[I] = "0123456789abcdef"[(Value >> (4 * (N - 1 - I))) & 0xF];
  }

  Serial[N] = '\0';
  return TRUE;
}

CONST CHAR8 *
EFIAPI
BootPolicyScreenDtbName (
  IN CONST CHAR8  *HwRev
  )
{
  if (Same (HwRev, Len (HwRev), "rg351mp")) {
    return "rg351mp-uboot.dtb";
  }

  if (Same (HwRev, Len (HwRev), "rg351v")) {
    return "rg351v-uboot.dtb";
  }

  return "rg351p-uboot.dtb";
}

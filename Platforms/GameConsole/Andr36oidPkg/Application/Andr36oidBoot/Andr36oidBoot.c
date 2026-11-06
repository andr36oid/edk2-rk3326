/** @file
  andr36oid boot app: does what BOOT/boot.ini does under U-Boot (boot.ini-android
  in the device repo) and starts Android's kernel directly.

    facts      U-Boot's (andr36oid.* on the bootloader FDT's command line,
               the chainload hook), else the board's own (RAM, buttons)
    keys       FN: recovery, Select: this menu, Start+Select: safe start
    BOOT       the SD card's FAT partition (the eMMC's for an eMMC install)
    files      boot.ini (the device profile's arguments), kernel-tweaks.txt,
               recovery.txt, emmc-test.txt, datacard.txt
    start      Image + ramdisk + rk3326-r36s-android.dtb through LinuxBootLib

  The log goes to BOOT/uefi-log.txt before the kernel starts and on every
  failure; a failure opens the menu.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Andr36oidBoot.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BootKeysLib.h>
#include <Library/DebugLib.h>
#include <Library/LinuxBootLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define KERNEL_MAX   SIZE_64MB
#define RAMDISK_MAX  SIZE_64MB
#define DTB_MAX      SIZE_1MB

VOID
EFIAPI
Say (
  IN CONST CHAR8  *Format,
  ...
  )
{
  // Long enough for the whole kernel command line (UefiLib's Print stops at 320).
  STATIC CHAR8   Line[BOOT_ARGS_MAX + 256];
  STATIC CHAR16  Wide[2 * ARRAY_SIZE (Line)];
  VA_LIST        Args;
  UINTN          I;
  UINTN          N = 0;

  VA_START (Args, Format);
  AsciiVSPrint (Line, sizeof (Line), Format, Args);
  VA_END (Args);

  // Into the RAM log (on the serial console the text already lands there),
  // in pieces: DebugLib cuts messages at 256 bytes.
  if (!FeaturePcdGet (PcdBootSerialConsole)) {
    for (I = 0; Line[I] != '\0'; I += N) {
      CHAR8  Piece[201];

      N = AsciiStrnLenS (Line + I, sizeof (Piece) - 1);
      CopyMem (Piece, Line + I, N);
      Piece[N] = '\0';
      DEBUG ((DEBUG_INIT, "%a", Piece));
    }

    N = 0;
  }

  for (I = 0; Line[I] != '\0'; I++) {
    if (Line[I] == '\n') {
      Wide[N++] = L'\r';
    }

    Wide[N++] = (CHAR16)(UINT8)Line[I];
  }

  Wide[N] = L'\0';
  gST->ConOut->OutputString (gST->ConOut, Wide);
}

typedef struct {
  EFI_FILE_PROTOCOL    *Root;
  VOID                 *Data[4];
  UINTN                Size[4];
  BOOT_FILES           Files;
} BOOT_VOLUME;

STATIC CONST CHAR8  *mTextFiles[4] = {
  BOOT_FILE_BOOT_INI,
  BOOT_FILE_KERNEL_TWEAKS,
  BOOT_FILE_DATACARD,
  BOOT_FILE_EMMC_TEST
};

STATIC
VOID
VolumeFree (
  IN OUT BOOT_VOLUME  *Vol
  )
{
  UINTN  I;

  for (I = 0; I < ARRAY_SIZE (Vol->Data); I++) {
    if (Vol->Data[I] != NULL) {
      FreePool (Vol->Data[I]);
    }
  }

  ZeroMem (Vol->Data, sizeof (Vol->Data));
  ZeroMem (Vol->Size, sizeof (Vol->Size));
  ZeroMem (&Vol->Files, sizeof (Vol->Files));
}

//
// The small files boot.ini reads, from Vol->Root.
//
STATIC
VOID
VolumeLoad (
  IN OUT BOOT_VOLUME  *Vol
  )
{
  UINTN  I;

  VolumeFree (Vol);
  for (I = 0; I < ARRAY_SIZE (mTextFiles); I++) {
    BootReadFile (Vol->Root, mTextFiles[I], BOOT_TEXT_MAX, &Vol->Data[I], &Vol->Size[I]);
  }

  Vol->Files.BootIni.Data      = Vol->Data[0];
  Vol->Files.BootIni.Size      = Vol->Size[0];
  Vol->Files.KernelTweaks.Data = Vol->Data[1];
  Vol->Files.KernelTweaks.Size = Vol->Size[1];
  Vol->Files.Datacard.Data     = Vol->Data[2];
  Vol->Files.Datacard.Size     = Vol->Size[2];
  Vol->Files.EmmcTest.Data     = Vol->Data[3];
  Vol->Files.EmmcTest.Size     = Vol->Size[3];
  Vol->Files.RecoveryTxt       = BootFileExists (Vol->Root, BOOT_FILE_RECOVERY) ||
                                 BootFileExists (Vol->Root, BOOT_FILE_RECOVERY_WIN);
}

STATIC
VOID
GatherFacts (
  IN  CONST BOOT_PLATFORM_INFO  *Pi,
  OUT BOOT_FACTS                *Facts
  )
{
  CONST CHAR8  *Args = NULL;

  ZeroMem (Facts, sizeof (*Facts));
  if (Pi->BootloaderFdt != 0) {
    Args = LinuxFdtGetBootargs ((VOID *)(UINTN)Pi->BootloaderFdt);
  }

  if (BootPolicyParseUbootArgs (Args, Facts)) {
    Say ("boot: U-Boot says hwrev=%a ram_mb=%u bootpart=%a\n", Facts->HwRev, Facts->RamMb, Facts->BootFromEmmc ? "0:1" : "1:1");
  } else {
    Facts->BootFromEmmc = (Pi->BootDevice == BootDeviceEmmc);
    Say ("boot: no facts from U-Boot, using the board's\n");
  }

  if (Facts->HwRev[0] == '\0') {
    switch (BootKeysGetBoardFamily ()) {
      case BoardFamilyRg351mp:
        AsciiStrCpyS (Facts->HwRev, sizeof (Facts->HwRev), "rg351mp");
        break;
      case BoardFamilyRg351v:
        AsciiStrCpyS (Facts->HwRev, sizeof (Facts->HwRev), "rg351v");
        break;
      case BoardFamilyRg351p:
        AsciiStrCpyS (Facts->HwRev, sizeof (Facts->HwRev), "rg351p");
        break;
      default:
        AsciiStrCpyS (Facts->HwRev, sizeof (Facts->HwRev), "v00");
        break;
    }
  }

  if ((Facts->RamMb == 0) && (Pi->DramSize != 0)) {
    Facts->RamMb = (UINT32)(Pi->DramSize >> 20);
  }

  if (Facts->SerialNo[0] == '\0') {
    Say ("boot: no serial number (U-Boot gives it), androidboot.serialno stays empty\n");
  }
}

//
// eMMC Installation step 2 (boot.ini's emmc-test.txt): mark the test tried on
// the SD card, then start from the eMMC once. Without an eMMC the UEFI can
// read the test is left for a later start through U-Boot.
//
STATIC
BOOLEAN
EmmcTest (
  IN OUT BOOT_VOLUME  *Sd,
  IN OUT BOOT_FACTS   *Facts
  )
{
  STATIC CONST CHAR8  Tried[] = "emmc_test=tried\n";
  EFI_FILE_PROTOCOL   *Emmc;
  CHAR8               Value[16];

  Emmc = BootOpenVolume (BootDeviceEmmc);
  if (Emmc == NULL) {
    Say ("boot: eMMC test: no eMMC BOOT here, left for a start through U-Boot\n");
    return FALSE;
  }

  if (!BootFileExists (Emmc, BOOT_FILE_BOOT_INI) || !BootFileExists (Emmc, BOOT_FILE_DTB) ||
      EFI_ERROR (BootWriteFile (Sd->Root, BOOT_FILE_EMMC_TEST, Tried, sizeof (Tried) - 1)))
  {
    Say ("boot: eMMC test: eMMC BOOT incomplete or emmc-test.txt not writable\n");
    Emmc->Close (Emmc);
    return FALSE;
  }

  VolumeLoad (Sd);
  if (!BootPolicyEnvGet (&Sd->Files.EmmcTest, "emmc_test", Value, sizeof (Value)) || (AsciiStrCmp (Value, "tried") != 0)) {
    Say ("boot: eMMC test: emmc-test.txt did not read back\n");
    Emmc->Close (Emmc);
    return FALSE;
  }

  Say ("boot: eMMC test: starting from the eMMC\n");
  Sd->Root            = Emmc;
  Facts->BootFromEmmc = TRUE;
  VolumeLoad (Sd);
  return TRUE;
}

//
// Load, place and start. Returns only on failure, with Message set.
//
STATIC
EFI_STATUS
StartLinux (
  IN  BOOT_VOLUME               *Vol,
  IN  CONST BOOT_PLAN           *Plan,
  IN  CONST BOOT_PLATFORM_INFO  *Pi,
  OUT CHAR8                     *Message,
  IN  UINTN                     MessageSize
  )
{
  EFI_STATUS          Status;
  LINUX_BOOT_PARAMS   Params;
  LINUX_BOOT_HANDOFF  Handoff;
  LINUX_MEMORY_RANGE  Memory[LINUX_MAX_MEMORY_RANGES];
  VOID                *Kernel  = NULL;
  VOID                *Ramdisk = NULL;
  VOID                *Dtb     = NULL;
  UINTN               KernelSize;
  UINTN               RamdiskSize;
  UINTN               DtbSize;
  UINTN               Offset;
  UINTN               Payload;
  CONST CHAR8         *Failed;
  CONST VOID          *Handover = (Pi->BootloaderFdt != 0) ? (VOID *)(UINTN)Pi->BootloaderFdt : NULL;

  ZeroMem (&Params, sizeof (Params));
  Failed = Plan->Kernel;
  Status = BootReadFile (Vol->Root, Plan->Kernel, KERNEL_MAX, &Kernel, &KernelSize);
  if (!EFI_ERROR (Status)) {
    Failed = Plan->Ramdisk;
    Status = BootReadFile (Vol->Root, Plan->Ramdisk, RAMDISK_MAX, &Ramdisk, &RamdiskSize);
  }

  if (!EFI_ERROR (Status)) {
    Failed = Plan->Dtb;
    Status = BootReadFile (Vol->Root, Plan->Dtb, DTB_MAX, &Dtb, &DtbSize);
  }

  if (EFI_ERROR (Status)) {
    AsciiSPrint (Message, MessageSize, "Could not read BOOT/%a (%r)", Failed, Status);
    goto Done;
  }

  Say ("boot: %a %lu, %a %lu, %a %lu bytes\n", Plan->Kernel, (UINT64)KernelSize, Plan->Ramdisk, (UINT64)RamdiskSize, Plan->Dtb, (UINT64)DtbSize);

  // ramdisk*.uimg are legacy uImages (U-Boot's booti wants them); a bare
  // cpio works too.
  Params.Initrd     = Ramdisk;
  Params.InitrdSize = RamdiskSize;
  Status            = LinuxUImagePayload (Ramdisk, RamdiskSize, &Offset, &Payload);
  if (Status == RETURN_SUCCESS) {
    Params.Initrd     = (UINT8 *)Ramdisk + Offset;
    Params.InitrdSize = Payload;
  } else if (Status != RETURN_UNSUPPORTED) {
    AsciiSPrint (Message, MessageSize, "BOOT/%a is a broken uImage", Plan->Ramdisk);
    Status = EFI_VOLUME_CORRUPTED;
    goto Done;
  }

  Params.Kernel     = Kernel;
  Params.KernelSize = KernelSize;
  Params.Fdt        = Dtb;
  Params.Fixup.Bootargs = Plan->Bootargs;

  //
  // RAM for Linux: the banks U-Boot wrote into its FDT (what U-Boot's booti
  // would have given Linux), else the platform's map.
  //
  Params.Fixup.MemoryCount = (Handover != NULL) ? LinuxFdtGetMemory (Handover, Memory, ARRAY_SIZE (Memory)) : 0;
  Params.Fixup.Memory      = Memory;
  if (Params.Fixup.MemoryCount != 0) {
    Say ("boot: RAM for Linux from the bootloader FDT: %u bank(s), first 0x%lx+0x%lx\n", Params.Fixup.MemoryCount, Memory[0].Base, Memory[0].Size);
  } else if (Pi->MemoryCount != 0) {
    Params.Fixup.Memory      = Pi->Memory;
    Params.Fixup.MemoryCount = Pi->MemoryCount;
    Say ("boot: RAM for Linux from the platform: %u bank(s), first 0x%lx+0x%lx\n", Pi->MemoryCount, Pi->Memory[0].Base, Pi->Memory[0].Size);
  } else {
    Params.Fixup.Memory = NULL;
    Say ("boot: RAM size unknown, the DTB's /memory stays\n");
  }

  // Boot logo handover: from U-Boot's FDT, else the live framebuffer
  Params.Fixup.HandoverFdt = Handover;
  if (Pi->FbValid) {
    Params.Fixup.FbBase = Pi->FbBase;
    Params.Fixup.FbSize = Pi->FbSize;
  }

  Status = LinuxBootPrepare (&Params, &Handoff);
  if (EFI_ERROR (Status)) {
    AsciiSPrint (Message, MessageSize, "Could not place the kernel (%r), see the log", Status);
    goto Done;
  }

  Say ("boot: kernel at 0x%lx, initrd 0x%lx+0x%lx, FDT 0x%lx\n", Handoff.KernelEntry, Handoff.InitrdBase, Handoff.InitrdSize, Handoff.Fdt);
  Say ("boot: bootargs: %a\n", Plan->Bootargs);
  Say ("boot: starting Linux\n");
  BootSaveLogs (Vol->Root);

  Status = LinuxBootStart (gImageHandle, &Handoff);
  // Only here if ExitBootServices failed: boot services are still up.
  LinuxBootFree (&Handoff);
  AsciiSPrint (Message, MessageSize, "Could not start the kernel (%r)", Status);

Done:
  if (Kernel != NULL) {
    FreePool (Kernel);
  }

  if (Ramdisk != NULL) {
    FreePool (Ramdisk);
  }

  if (Dtb != NULL) {
    FreePool (Dtb);
  }

  return Status;
}

EFI_STATUS
EFIAPI
Andr36oidBootMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  BOOT_PLATFORM_INFO  Pi;
  BOOT_FACTS          Facts;
  BOOT_KEYS           Keys;
  BOOT_VOLUME         Vol;
  BOOT_PLAN           *Plan;
  BOOT_CHOICE         Choice = BootChoiceNone;
  EFI_INPUT_KEY       Key;
  BOOLEAN             KeyMenu = FALSE;
  CHAR8               Message[160];

  Plan = AllocateZeroPool (sizeof (*Plan));
  if (Plan == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (&Vol, sizeof (Vol));
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Say ("andr36oid boot\n");

  if (EFI_ERROR (BootPlatformGetInfo (&Pi))) {
    Say ("boot: no platform info\n");
  }

  GatherFacts (&Pi, &Facts);

  // Buttons held at power-on, as boot.ini reads them
  Keys.Fn     = BootKeyPressed (BootKeyFn);
  Keys.Select = BootKeyPressed (BootKeySelect);
  Keys.Start  = BootKeyPressed (BootKeyStart);
  // A keyboard: Esc or m already typed opens the menu too.
  if ((gST->ConIn->ReadKeyStroke (gST->ConIn, &Key) == EFI_SUCCESS) &&
      ((Key.ScanCode == SCAN_ESC) || (Key.UnicodeChar == L'm') || (Key.UnicodeChar == L'M')))
  {
    KeyMenu = TRUE;
  }

  Say ("boot: board %a, hwrev %a, %u MB, keys fn=%d select=%d start=%d\n", Pi.Name, Facts.HwRev, Facts.RamMb, Keys.Fn, Keys.Select, Keys.Start);

  // Without BOOT only the menu is left (Save log has nowhere to go either).
  for ( ; ;) {
    Vol.Root = BootOpenVolume (Facts.BootFromEmmc ? BootDeviceEmmc : BootDeviceSd);
    if (Vol.Root != NULL) {
      break;
    }

    AsciiSPrint (Message, sizeof (Message), "No BOOT partition with Image or boot.ini on the %a", Facts.BootFromEmmc ? "eMMC" : "SD card");
    Say ("boot: %a\n", Message);
    Choice = BootMenu (NULL, Message);
  }

  VolumeLoad (&Vol);
  Say ("boot: BOOT: boot.ini %a, kernel-tweaks.txt %a, recovery.txt %a\n", Vol.Files.BootIni.Data ? "yes" : "no", Vol.Files.KernelTweaks.Data ? "yes" : "no", Vol.Files.RecoveryTxt ? "yes" : "no");

  for ( ; ;) {
    if (RETURN_ERROR (BootPolicyDecide (&Facts, &Keys, &Vol.Files, Choice, Plan))) {
      AsciiSPrint (Message, sizeof (Message), "The kernel command line is too long (boot.ini or kernel-tweaks.txt)");
      Say ("boot: %a\n", Message);
      BootSaveLogs (Vol.Root);
      Choice = BootMenu (Vol.Root, Message);
      continue;
    }

    if ((Choice == BootChoiceNone) && (Plan->ShowMenu || KeyMenu)) {
      Choice = BootMenu (Vol.Root, NULL);
      continue;
    }

    if (Plan->EmmcTest && EmmcTest (&Vol, &Facts)) {
      continue;     // decide again with the eMMC's files
    }

    Say (
      "boot: %a%a%a\n",
      (Plan->Target == BootTargetRecovery) ? "recovery" : "Android",
      Plan->Safe ? ", safe start" : "",
      Plan->BootLog ? ", boot log" : ""
      );

    if (Plan->LowRam) {
      gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_YELLOW, EFI_BLACK));
      Say ("\n  This console has less than 1 GB of RAM\n  (%u MB usable), though sellers\n  often claim 1 GB.\n  Android may start slowly or freeze.\n\n  Starting in 10 seconds...\n", Facts.RamMb);
      gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
      gBS->Stall (10 * 1000 * 1000);
    }

    StartLinux (&Vol, Plan, &Pi, Message, sizeof (Message));
    Say ("boot: %a\n", Message);
    BootSaveLogs (Vol.Root);
    Choice = BootMenu (Vol.Root, Message);
  }
}

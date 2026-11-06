// RK3326 ResetSystemLib against the PSCI spec (Arm DEN0022, function IDs and
// return codes written out here, not taken from ArmStdSmc.h) and the
// library's own rule for shutdown: the RK817 first (DEV_OFF, as U-Boot and
// the kernel do), PSCI SYSTEM_OFF only if the PMIC did not cut the power.
// The firmware monitor is a fake that records every call; a monitor call
// that "does not return" ends the case through longjmp.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent

#include <setjmp.h>
#include <PiDxe.h>
#include <IndustryStandard/ArmStdSmc.h>
#include <Library/ArmMonitorLib.h>
#include <Library/RkPmicLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"

// PSCI (DEN0022D table 5.1, 5.2)
#define PSCI_SYSTEM_OFF          0x84000008U
#define PSCI_SYSTEM_RESET        0x84000009U
#define PSCI_FEATURES            0x8400000AU
#define PSCI_SYSTEM_RESET2_64    0xC4000012U
#define PSCI_SUCCESS             0
#define PSCI_NOT_SUPPORTED       (-1)
#define SYSTEM_WARM_RESET        0           // SYSTEM_RESET2 reset_type 0 (bit 31 clear: architectural)

EFI_BOOT_SERVICES  *gBS;
EFI_HANDLE         gImageHandle;
EFI_SYSTEM_TABLE   *gST;

static jmp_buf  Gone;
static int      nCalls, nPmic;
static UINTN    Calls[8][3];
static INTN     FeatureReply;       // PSCI_FEATURES(SYSTEM_RESET2) answer
static BOOLEAN  ResetReturns;       // the monitor comes back from a reset (broken BL31)
static BOOLEAN  PmicCutsPower;

VOID EFIAPI ArmMonitorCall (IN OUT ARM_MONITOR_ARGS *Args)
{
  UINTN  Fid = Args->Arg0 & 0xFFFFFFFF;

  if (nCalls < 8) {
    Calls[nCalls][0] = Fid;
    Calls[nCalls][1] = Args->Arg1;
    Calls[nCalls][2] = Args->Arg2;
  }

  nCalls++;
  if (Fid == PSCI_FEATURES) {
    Args->Arg0 = (UINTN)FeatureReply;
    return;
  }

  if (((Fid == PSCI_SYSTEM_RESET) || (Fid == PSCI_SYSTEM_RESET2_64) || (Fid == PSCI_SYSTEM_OFF)) && !ResetReturns) {
    longjmp (Gone, 1);
  }

  Args->Arg0 = (UINTN)(INTN)PSCI_NOT_SUPPORTED;
}

EFI_STATUS EFIAPI RkPmicPowerOff (VOID)
{
  nPmic++;
  if (PmicCutsPower) {
    longjmp (Gone, 2);
  }

  return EFI_DEVICE_ERROR;
}

#include "ResetSystemLib.c"

static void Run (EFI_RESET_TYPE Type)
{
  nCalls = 0;
  nPmic  = 0;
  memset (Calls, 0, sizeof (Calls));
  if (setjmp (Gone) == 0) {
    ResetSystem (Type, EFI_SUCCESS, 0, NULL);
  }
}

int main (void)
{
  // The IDs this library uses are the spec's.
  CHECK_EQ (ARM_SMC_ID_PSCI_SYSTEM_OFF, PSCI_SYSTEM_OFF);
  CHECK_EQ (ARM_SMC_ID_PSCI_SYSTEM_RESET, PSCI_SYSTEM_RESET);
  CHECK_EQ (ARM_SMC_ID_PSCI_FEATURES, PSCI_FEATURES);
  CHECK_EQ (ARM_SMC_ID_PSCI_SYSTEM_RESET2_AARCH64, PSCI_SYSTEM_RESET2_64);
  CHECK_EQ (RkResetSystemLibConstructor (), EFI_SUCCESS);

  // Cold: SYSTEM_RESET, nothing else.
  FeatureReply = PSCI_SUCCESS;
  Run (EfiResetCold);
  CHECK_EQ (nCalls, 1);
  CHECK_EQ (Calls[0][0], PSCI_SYSTEM_RESET);
  CHECK_EQ (nPmic, 0);

  // Warm with SYSTEM_RESET2: asked first, then a warm reset of type 0.
  Run (EfiResetWarm);
  CHECK_EQ (nCalls, 2);
  CHECK_EQ (Calls[0][0], PSCI_FEATURES);
  CHECK_EQ (Calls[0][1], PSCI_SYSTEM_RESET2_64);
  CHECK_EQ (Calls[1][0], PSCI_SYSTEM_RESET2_64);
  CHECK_EQ (Calls[1][1], SYSTEM_WARM_RESET);

  // Warm without it (Rockchip's BL31 may not have it): a cold reset.
  FeatureReply = PSCI_NOT_SUPPORTED;
  Run (EfiResetWarm);
  CHECK_EQ (nCalls, 2);
  CHECK_EQ (Calls[0][0], PSCI_FEATURES);
  CHECK_EQ (Calls[1][0], PSCI_SYSTEM_RESET);

  // Shutdown: the PMIC, and nothing after it when the power goes.
  PmicCutsPower = TRUE;
  Run (EfiResetShutdown);
  CHECK_EQ (nPmic, 1);
  CHECK_EQ (nCalls, 0);

  // The PMIC does not answer: PSCI SYSTEM_OFF.
  PmicCutsPower = FALSE;
  Run (EfiResetShutdown);
  CHECK_EQ (nPmic, 1);
  CHECK_EQ (nCalls, 1);
  CHECK_EQ (Calls[0][0], PSCI_SYSTEM_OFF);

  // Platform specific: a reboot.
  Run (EfiResetPlatformSpecific);
  CHECK_EQ (nCalls, 1);
  CHECK_EQ (Calls[0][0], PSCI_SYSTEM_RESET);

  // Unknown types do nothing (and return).
  Run ((EFI_RESET_TYPE)7);
  CHECK_EQ (nCalls, 0);
  CHECK_EQ (nPmic, 0);

  // A monitor that comes back from SYSTEM_RESET2: report what happens (the
  // UEFI caller then dead-loops; ResetSystem must not have done anything else).
  FeatureReply = PSCI_SUCCESS;
  ResetReturns = TRUE;
  Run (EfiResetWarm);
  printf ("note: if SYSTEM_RESET2 returns, the warm reset %s\n",
          nCalls > 2 ? "falls back to another reset" : "returns to the caller");
  ResetReturns = FALSE;

  DONE ();
}

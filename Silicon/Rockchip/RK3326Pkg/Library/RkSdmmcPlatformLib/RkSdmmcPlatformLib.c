/** @file
 *
 *  RkSdmmcDxe platform helper library.
 *
 *  Copyright (c) 2023, Mario Bălănică <mariobalanica02@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Uefi.h>
#include <Library/RkSdmmcPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/GpioLib.h>
#include <Library/IoLib.h>

STATIC CONST GPIO_IOMUX_CONFIG mSdmmc0IomuxConfig[] = {
  { "sdmmc0_clk",         1, GPIO_PIN_PD6, 1, GPIO_PIN_PULL_NONE, GPIO_PIN_DRIVE_DEFAULT },
  { "sdmmc0_cmd",         1, GPIO_PIN_PD7, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
  { "sdmmc0_det",         0, GPIO_PIN_PA3, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
  { "sdmmc_bus4",         1, GPIO_PIN_PD2, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
  { "sdmmc_bus4_1",       1, GPIO_PIN_PD3, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
  { "sdmmc_bus4_2",       1, GPIO_PIN_PD4, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
  { "sdmmc_bus4_3",       1, GPIO_PIN_PD5, 1, GPIO_PIN_PULL_UP, GPIO_PIN_DRIVE_3 },
};
VOID
EFIAPI
RkSdmmcSetIoMux (
  VOID
  )
{
  GpioPinSetDirection (0, GPIO_PIN_PA3, GPIO_PIN_INPUT);
  GpioSetIomuxConfig (mSdmmc0IomuxConfig, ARRAY_SIZE (mSdmmc0IomuxConfig));
}

RKSDMMC_CARD_PRESENCE_STATE
EFIAPI
RkSdmmcGetCardPresenceState (
  VOID
  )
{
  // cd-gpios = <&gpio0 RK_PA3 GPIO_ACTIVE_LOW>: a card pulls the pin low.
  return GpioPinGetLevel (0, GPIO_PIN_PA3) ? RkSdmmcCardNotPresent
                                           : RkSdmmcCardPresent;
}

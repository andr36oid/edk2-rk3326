/** @file
  SARADC single-shot read. Register use follows U-Boot's rockchip_saradc.

  SPDX-License-Identifier: GPL-2.0+
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/CruLib.h>
#include <Library/RkSaradcLib.h>

#define SARADC_BASE         0xFF288000UL
#define SARADC_DATA         0x00
#define SARADC_CTRL         0x08
#define SARADC_DLY_PU_SOC   0x0C

#define CTRL_CHN_MASK       0x7
#define CTRL_POWER          BIT3
#define CTRL_IRQ_ENABLE     BIT5
#define CTRL_IRQ_STATUS     BIT6
#define DATA_MASK           0x3FF

#define SARADC_CLOCK_HZ     1000000
#define SARADC_TIMEOUT_US   20000

EFI_STATUS
EFIAPI
RkSaradcRead (
  IN  UINT8   Channel,
  OUT UINT32  *Value
  )
{
  UINTN       Left;
  EFI_STATUS  Status;

  if ((Channel > 2) || (Value == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  CruEnableSaradcClock (SARADC_CLOCK_HZ);

  //
  // 8 clocks between power-up and the start of the conversion.
  //
  MmioWrite32 (SARADC_BASE + SARADC_DLY_PU_SOC, 8);
  MmioWrite32 (SARADC_BASE + SARADC_CTRL, CTRL_POWER | CTRL_IRQ_ENABLE | (Channel & CTRL_CHN_MASK));

  Status = EFI_TIMEOUT;
  for (Left = SARADC_TIMEOUT_US; Left > 0; Left--) {
    if ((MmioRead32 (SARADC_BASE + SARADC_CTRL) & CTRL_IRQ_STATUS) != 0) {
      *Value = MmioRead32 (SARADC_BASE + SARADC_DATA) & DATA_MASK;
      Status = EFI_SUCCESS;
      break;
    }

    MicroSecondDelay (1);
  }

  MmioWrite32 (SARADC_BASE + SARADC_CTRL, 0);
  return Status;
}

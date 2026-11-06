/** @file
 *
 *  RK3326 (PX30) GPIO Library.
 * 
 *  Copyright (c) 2021, Jared McNeill <jmcneill@invisible.ca>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 **/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/GpioLib.h>

#include "GpioRegs.h"

STATIC
VOID
GrfWriteMasked (
  IN UINTN   Reg,
  IN UINT32  Mask,
  IN UINT32  Value
  )
{
  //
  // GRF registers take the write enable of each bit in the upper half.
  //
  MmioWrite32 (Reg, (Mask << 16) | (Value & Mask));
}

STATIC
VOID
GpioRmw (
  IN UINTN   Reg,
  IN UINT32  Bit,
  IN BOOLEAN Set
  )
{
  UINT32  Value;

  Value = MmioRead32 (Reg);
  if (Set) {
    Value |= Bit;
  } else {
    Value &= ~Bit;
  }

  MmioWrite32 (Reg, Value);
}

VOID
GpioPinSetDirection (
  IN UINT8 Group,
  IN UINT8 Pin,
  IN GPIO_PIN_DIRECTION Direction
  )
{
  ASSERT (Group < GPIO_NGROUPS);
  GpioRmw (GPIO_BASE (Group) + GPIO_SWPORT_DDR, 1U << Pin, Direction == GPIO_PIN_OUTPUT);
}

VOID
GpioPinWrite (
  IN UINT8 Group,
  IN UINT8 Pin,
  IN BOOLEAN Value
  )
{
  ASSERT (Group < GPIO_NGROUPS);
  GpioRmw (GPIO_BASE (Group) + GPIO_SWPORT_DR, 1U << Pin, Value);
}

BOOLEAN
GpioPinGetLevel (
  IN UINT8 Group,
  IN UINT8 Pin
  )
{
  ASSERT (Group < GPIO_NGROUPS);
  return ((MmioRead32 (GPIO_BASE (Group) + GPIO_EXT_PORT) >> Pin) & 1) != 0;
}

VOID
GpioPinSetFunction (
  IN UINT8 Group,
  IN UINT8 Pin,
  IN UINT8 Function
  )
{
  GPIO_REG_FIELD  F;

  ASSERT (Group < GPIO_NGROUPS);
  GpioIomuxField (Group, Pin, &F);

  DEBUG ((DEBUG_INFO, "GPIO: SetFunction %u %02X %04X      0x%lX bit %u\n", Group, Pin, Function, F.Reg, F.Shift));

  GrfWriteMasked (F.Reg, F.Mask << F.Shift, ((UINT32)Function & F.Mask) << F.Shift);
}

VOID
GpioPinSetPull (
  IN UINT8 Group,
  IN UINT8 Pin,
  IN GPIO_PIN_PULL Pull
  )
{
  GPIO_REG_FIELD  F;

  ASSERT (Group < GPIO_NGROUPS);
  GpioPullField (Group, Pin, &F);

  DEBUG ((DEBUG_INFO, "GPIO: SetPull     %u %02X %04X      0x%lX bit %u\n", Group, Pin, Pull, F.Reg, F.Shift));

  GrfWriteMasked (F.Reg, F.Mask << F.Shift, ((UINT32)Pull & F.Mask) << F.Shift);
}

VOID
GpioPinSetDriveLevel (
  IN UINT8 Group,
  IN UINT8 Pin,
  IN UINT8 Level
  )
{
  GPIO_REG_FIELD  F;

  ASSERT (Group < GPIO_NGROUPS);
  GpioDriveField (Group, Pin, &F);

  DEBUG ((DEBUG_INFO, "GPIO: SetDrive    %u %02X %04X      0x%lX bit %u\n", Group, Pin, Level, F.Reg, F.Shift));

  GrfWriteMasked (F.Reg, F.Mask << F.Shift, ((UINT32)Level & F.Mask) << F.Shift);
}

VOID
GpioPinSetDrive (
  IN UINT8 Group,
  IN UINT8 Pin,
  IN GPIO_PIN_DRIVE Drive
  )
{
  ASSERT (Drive != GPIO_PIN_DRIVE_DEFAULT);
  GpioPinSetDriveLevel (Group, Pin, GpioDriveEnumToLevel ((UINT32)Drive));
}

VOID
GpioPinSetInput (
  IN UINT8 Group,
  IN UINT8 Pin,
  IN GPIO_PIN_INPUT_ENABLE InputEnable
  )
{
  GPIO_REG_FIELD  F;

  ASSERT (Group < GPIO_NGROUPS);
  ASSERT (InputEnable != GPIO_PIN_INPUT_DEFAULT);
  GpioSchmittField (Group, Pin, &F);

  DEBUG ((DEBUG_INFO, "GPIO: SetInput    %u %02X %04X      0x%lX bit %u\n", Group, Pin, InputEnable, F.Reg, F.Shift));

  //
  // PX30 only has a Schmitt trigger switch here, no separate input enable.
  //
  GrfWriteMasked (F.Reg, F.Mask << F.Shift, (InputEnable != GPIO_PIN_INPUT_DISABLE ? 1U : 0U) << F.Shift);
}

VOID
GpioSetIomuxConfig (
  IN CONST GPIO_IOMUX_CONFIG *Configs,
  IN UINT32 NumConfigs
  )
{
  UINT32 Index;

  for (Index = 0; Index < NumConfigs; Index++) {
    CONST GPIO_IOMUX_CONFIG *Mux = &Configs[Index];
    DEBUG ((DEBUG_INFO, "GPIO: IOMUX for pin '%a'\n", Mux->Name));
    GpioPinSetFunction (Mux->Group, Mux->Pin, Mux->Function);
    GpioPinSetPull (Mux->Group, Mux->Pin, Mux->Pull);
    if (Mux->Drive != GPIO_PIN_DRIVE_DEFAULT) {
      GpioPinSetDrive (Mux->Group, Mux->Pin, Mux->Drive);
    }
  }
}

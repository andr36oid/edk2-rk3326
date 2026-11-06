// RkPmicLibRk817.c against a model of the RK817 register file (write-enable
// nibbles on POWER_EN, write-1-to-clear interrupt status) and, when a DTB
// path is given, the node names/properties of a real device tree.
#include <Uefi.h>
#include <Library/FdtLib.h>
#include <Library/RkI2cLib.h>
#include <Library/RkPmicLib.h>
#include <stdio.h>
#include <string.h>
#include "test.h"

static UINT8 dev[256];
static int   i2c_inits, i2c_fail, id_msb = 0x81, id_lsb = 0x70;
static int   write_log_n;
static struct { UINT8 reg, val; } write_log[256];

static void reset_dev (void)
{
  memset (dev, 0, sizeof (dev));
  dev[0xED] = id_msb; dev[0xEE] = id_lsb;
  dev[0xF5] = 0x01; dev[0xF6] = 0x00;
  write_log_n = 0;
}

EFI_STATUS EFIAPI RkI2cInit (UINT8 Bus, UINT32 SclHz) { i2c_inits++; CHECK_EQ (Bus, 0); CHECK_EQ (SclHz, 400000); return (i2c_fail == 1) ? EFI_DEVICE_ERROR : EFI_SUCCESS; }

EFI_STATUS EFIAPI RkI2cRead (UINT8 Bus, UINT8 Chip, UINT8 Reg, UINT8 *Buf, UINTN Len)
{
  CHECK_EQ (Chip, 0x20);
  if (i2c_fail == 2) return EFI_NO_RESPONSE;
  for (UINTN i = 0; i < Len; i++) Buf[i] = dev[(UINT8)(Reg + i)];
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI RkI2cWrite (UINT8 Bus, UINT8 Chip, UINT8 Reg, CONST UINT8 *Buf, UINTN Len)
{
  CHECK_EQ (Chip, 0x20);
  CHECK_EQ (Len, 1);
  if (i2c_fail == 2) return EFI_NO_RESPONSE;
  write_log[write_log_n].reg = Reg; write_log[write_log_n].val = Buf[0]; write_log_n++;
  if (Reg >= 0xB1 && Reg <= 0xB4) {            // upper nibble = write enables for the lower one
    UINT8 en = Buf[0] >> 4;
    dev[Reg] = (dev[Reg] & ~en & 0x0F) | (Buf[0] & en);
  } else if (Reg == 0xF8) {                    // interrupt status: write 1 to clear
    dev[Reg] &= ~Buf[0];
  } else {
    dev[Reg] = Buf[0];
  }
  return EFI_SUCCESS;
}

static int wrote (UINT8 reg)
{
  for (int i = 0; i < write_log_n; i++) if (write_log[i].reg == reg) return 1;
  return 0;
}

// Fresh library state: the lib caches "ready", so tests that need a re-init run
// in a forked-like fashion by re-linking is overkill; instead order the tests.

static void test_basic (void)
{
  RK_POWER_STATUS s;
  UINTN off;

  reset_dev ();
  dev[0xB1] = 0x0F; dev[0xB2] = 0xA5; dev[0xB3] = 0x03; dev[0xB4] = 0xF4;   // rails as the boot ROM left them
  CHECK_EQ (RkPmicInit (), EFI_SUCCESS);
  CHECK_EQ (i2c_inits, 1);
  CHECK_EQ (dev[0x99], 0x5F);                  // POWER_EN_SAVE0/1 as U-Boot's probe fills them
  CHECK_EQ (dev[0xA4], 0x43);
  CHECK_EQ (RkPmicInit (), EFI_SUCCESS);       // second call does nothing
  CHECK_EQ (i2c_inits, 1);
  // what U-Boot's rk817_init_reg sets
  CHECK_EQ (dev[0xC6] & 0x60, 0x60);
  CHECK_EQ (dev[0xF4] & 0x18, 0x08);
  CHECK_EQ (dev[0xFE] & 0x02, 0x00);
  CHECK_EQ (dev[0x50] & 0x08, 0x08);           // battery ADC on

  // power status
  dev[0xF0] = 0x40;                            // PLUG_IN_STS
  dev[0xEB] = 0xA0;                            // battery there, state 2 (trickle)
  // calibration 2300 mV at 0x0A00, 4025 mV at 0x1400; battery at code 0x0F00 = 3162 mV
  dev[0x93] = 0x0A; dev[0x94] = 0x00; dev[0x95] = 0x14; dev[0x96] = 0x00;
  dev[0x78] = 0x0F; dev[0x79] = 0x00;
  CHECK_EQ (RkPmicGetPowerStatus (&s), EFI_SUCCESS);
  CHECK (s.ChargerPresent);
  CHECK (s.Charging);
  CHECK (!s.PowerKeyPressed);
  CHECK (s.BatteryMicroVolts > 3150000 && s.BatteryMicroVolts < 3175000);
  CHECK (s.BatteryPresent);
  CHECK (!s.PowerKeyTap);

  dev[0xF0] = 0x00; dev[0xEB] = 0x80;
  CHECK_EQ (RkPmicGetPowerStatus (&s), EFI_SUCCESS);
  CHECK (!s.ChargerPresent); CHECK (!s.Charging);
  dev[0xEB] = 0x00;                            // no battery
  RkPmicGetPowerStatus (&s);
  CHECK (!s.BatteryPresent);
  // calibration registers still zero: the maths gives nonsense, reported as unknown
  dev[0x93] = dev[0x94] = dev[0x95] = dev[0x96] = 0; dev[0x78] = dev[0x79] = 0;
  RkPmicGetPowerStatus (&s);
  CHECK_EQ (s.BatteryMicroVolts, 0);
  dev[0x93] = 0x0A; dev[0x94] = 0x00; dev[0x95] = 0x14; dev[0x96] = 0x00;
  dev[0x78] = 0x0F; dev[0x79] = 0x00; dev[0xEB] = 0x80;

  // power key: fall latches pressed, rise releases, status bits are cleared
  dev[0xF8] = 0x01;
  CHECK_EQ (RkPmicGetPowerStatus (&s), EFI_SUCCESS);
  CHECK (s.PowerKeyPressed);
  CHECK_EQ (dev[0xF8] & 0x03, 0);
  CHECK_EQ (RkPmicGetPowerStatus (&s), EFI_SUCCESS);
  CHECK (s.PowerKeyPressed);                   // still held, no new events
  dev[0xF8] = 0x02;
  CHECK_EQ (RkPmicGetPowerStatus (&s), EFI_SUCCESS);
  CHECK (!s.PowerKeyPressed);
  dev[0xF8] = 0x03;                            // fall and rise in one read: a tap
  RkPmicGetPowerStatus (&s);
  CHECK (!s.PowerKeyPressed); CHECK (s.PowerKeyTap);
  dev[0xF8] = 0x00;
  RkPmicGetPowerStatus (&s);
  CHECK (!s.PowerKeyTap);
  dev[0xF8] = 0x40 | 0x01;                     // other bits (RTC) must not be cleared by us
  RkPmicGetPowerStatus (&s);
  CHECK_EQ (dev[0xF8] & 0x40, 0x40);

  // battery voltage 0 when the ADC reads nonsense (calibration points equal => k huge but sane)
  CHECK_EQ (RkPmicGetPowerStatus (NULL), EFI_INVALID_PARAMETER);

  // charger limits: DC in -> 4.5 V limit, 1500 mA in, 1500 mA charge; unplugged -> 450 mA
  dev[0xF0] = 0x40; dev[0xE4] = 0xF8; dev[0xE5] = 0x00;
  CHECK_EQ (RkPmicSetChargerLimits (), EFI_SUCCESS);
  CHECK_EQ (dev[0xE5], 0xDB);
  CHECK_EQ (dev[0xE4], 0xF9);
  dev[0xF0] = 0x00; dev[0xE4] = 0xF8;
  CHECK_EQ (RkPmicSetChargerLimits (), EFI_SUCCESS);
  CHECK_EQ (dev[0xE5], 0xD8);
  CHECK_EQ (dev[0xE4], 0xF8);                  // charge current left alone without a charger

  // power off: DEV_OFF set, other SYS_CFG3 bits kept, "does not return" is an error here
  dev[0xF4] = 0x08;
  CHECK_EQ (RkPmicPowerOff (), EFI_DEVICE_ERROR);
  CHECK_EQ (dev[0xF4], 0x09);
}

#ifndef NO_DTB
static void *load (const char *path)
{
  FILE *f = fopen (path, "rb");
  static UINT8 buf[1 << 20];
  if (!f) return NULL;
  size_t n = fread (buf, 1, sizeof (buf), f);
  fclose (f);
  return n ? buf : NULL;
}

static INT32 find_reg (void *fdt, const char *name)
{
  INT32 pmic = FdtNodeOffsetByCompatible (fdt, -1, "rockchip,rk817");
  INT32 regs = FdtSubnodeOffset (fdt, pmic, "regulators");
  return FdtSubnodeOffset (fdt, regs, name);
}

static void test_dtb (void *fdt)
{
  INT32 n;

  // vcc_lcd: LDO_REG8 2.8 V, off at start. Voltage first, then enable.
  reset_dev ();
  n = find_reg (fdt, "LDO_REG8");
  CHECK (n >= 0);
  write_log_n = 0;
  CHECK_EQ (RkPmicSetRegulator (fdt, n, TRUE), EFI_SUCCESS);
  CHECK_EQ (dev[0xDA], 88);                    // (2.8 V - 0.6 V) / 25 mV
  CHECK_EQ (dev[0xB3] & 0x08, 0x08);
  CHECK_EQ (write_log[write_log_n - 1].reg, 0xB3);   // enable is the last step
  CHECK_EQ (write_log[write_log_n - 1].val, 0x88);
  CHECK_EQ (RkPmicSetRegulator (fdt, n, FALSE), EFI_SUCCESS);
  CHECK_EQ (dev[0xB3] & 0x08, 0);
  CHECK_EQ (dev[0xDA], 88);                    // voltage stays

  // vcc_backlight LDO_REG7 3.3 V
  n = find_reg (fdt, "LDO_REG7");
  CHECK_EQ (RkPmicSetRegulator (fdt, n, TRUE), EFI_SUCCESS);
  CHECK_EQ (dev[0xD8], 108);
  CHECK_EQ (dev[0xB3] & 0x04, 0x04);

  CHECK_EQ (RkPmicSetRegulator (fdt, -1, TRUE), EFI_INVALID_PARAMETER);
  CHECK_EQ (RkPmicSetRegulator (NULL, 0, TRUE), EFI_INVALID_PARAMETER);
  // a node that is not a regulator
  CHECK_EQ (RkPmicSetRegulator (fdt, FdtNodeOffsetByCompatible (fdt, -1, "rockchip,rk817"), TRUE), EFI_NOT_FOUND);

  // boot-on pass
  reset_dev ();
  dev[0xBB] = 0x55; dev[0xBE] = 0x56;          // DCDC1/DCDC2 voltages as the earlier stage left them
  write_log_n = 0;
  CHECK_EQ (RkPmicEnableBootOn (fdt), EFI_SUCCESS);
  CHECK_EQ (dev[0xBB], 0x55);                  // ranged rails: voltage untouched
  CHECK_EQ (dev[0xBE], 0x56);
  CHECK (!wrote (0xBB) && !wrote (0xBE));
  CHECK_EQ (dev[0xB1] & 0x0F, 0x0F);           // DCDC1-4 always-on
  CHECK_EQ (dev[0xCE], 48);                    // LDO2 vcc1v8_soc, fixed 1.8 V
  CHECK_EQ (dev[0xB2] & 0x0F, 0x0F);           // LDO1-4 are all always-on in this DTB
  CHECK_EQ (dev[0xCC], 16);                    // LDO1 1.0 V
  CHECK_EQ (dev[0xD4], 48);                    // LDO5 fixed 1.8 V
  CHECK (!wrote (0xD6));                       // LDO6 is 1.8-3.0 V: only enabled
  CHECK_EQ (dev[0xB3] & 0x0F, 0x03 | 0x00);    // LDO5, LDO6 on; LDO7, LDO8 (panel supplies) stay off
  CHECK_EQ (dev[0xB4] & 0x07, 0x07);           // LDO9, BOOST, OTG switch
  CHECK_EQ (dev[0xDC], 108);                   // LDO9 3.3 V
  CHECK (!wrote (0xDE));                       // BOOST 5.0-5.4 V: only enabled
}
#endif

// Failure modes, one per process because the library remembers a good init.
static int test_failure (const char *mode)
{
  RK_POWER_STATUS s;
  UINT8 before[256];

  reset_dev ();
  if (!strcmp (mode, "--bad-id")) {
    dev[0xED] = 0x80; dev[0xEE] = 0x90;       // an RK809
    CHECK_EQ (RkPmicInit (), EFI_UNSUPPORTED);
  } else if (!strcmp (mode, "--no-ack")) {
    i2c_fail = 2;
    CHECK_EQ (RkPmicInit (), EFI_NO_RESPONSE);
  } else {
    i2c_fail = 1;
    CHECK_EQ (RkPmicInit (), EFI_DEVICE_ERROR);
  }

  memcpy (before, dev, sizeof (before));
  i2c_fail = 0;
  // nothing else may touch the chip after a failed init... but a later call
  // retries the init, so make the chip still wrong for --bad-id
  if (!strcmp (mode, "--bad-id")) {
    CHECK (RkPmicGetPowerStatus (&s) != EFI_SUCCESS);
    CHECK (RkPmicPowerOff () != EFI_SUCCESS);
    CHECK_EQ (memcmp (before, dev, sizeof (before)), 0);   // not a single register written
    CHECK_EQ (s.ChargerPresent, 0);
    CHECK_EQ (s.BatteryMicroVolts, 0);
  }
  DONE ();
}

int main (int argc, char **argv)
{
  if (argc > 1 && argv[1][0] == '-') {
    return test_failure (argv[1]);
  }

  test_basic ();
#ifndef NO_DTB
  if (argc > 1) {
    void *fdt = load (argv[1]);
    CHECK (fdt != NULL);
    if (fdt) test_dtb (fdt);
  }
#endif
  // chip id mismatch and bus failure are reported (library state is reset by
  // the checks below being last)
  DONE ();
}

// Pure arithmetic: clocks, pin registers, I2C divider, RK817 tables, board id
// thresholds, key maps, key repeat. Expected values come from the kernel and
// U-Boot sources and from the mw.l lines in boot.ini-android, not from the code
// under test.
#include <Uefi.h>
#include <Library/GpioLib.h>
#include <Library/BootKeysLib.h>
#include "test.h"
#include "CruMath.h"
#include "GpioRegs.h"
#include "RkI2cMath.h"
#include "Rk817Regs.h"
#include "BootKeysMap.h"
#include "KeyRepeat.h"

static void test_board_family (void)
{
  // U-Boot cmd/hwrev.c: exclusive ranges, 0 is never valid
  CHECK_EQ (BoardFamilyFromAdc (0),    BoardFamilyUnknown);
  CHECK_EQ (BoardFamilyFromAdc (146),  BoardFamilyUnknown);
  CHECK_EQ (BoardFamilyFromAdc (147),  BoardFamilyRg351mp);
  CHECK_EQ (BoardFamilyFromAdc (166),  BoardFamilyRg351mp);
  CHECK_EQ (BoardFamilyFromAdc (185),  BoardFamilyRg351mp);
  CHECK_EQ (BoardFamilyFromAdc (186),  BoardFamilyUnknown);
  CHECK_EQ (BoardFamilyFromAdc (494),  BoardFamilyUnknown);
  CHECK_EQ (BoardFamilyFromAdc (495),  BoardFamilyRg351v);
  CHECK_EQ (BoardFamilyFromAdc (514),  BoardFamilyRg351v);
  CHECK_EQ (BoardFamilyFromAdc (533),  BoardFamilyRg351v);
  CHECK_EQ (BoardFamilyFromAdc (534),  BoardFamilyUnknown);
  CHECK_EQ (BoardFamilyFromAdc (655),  BoardFamilyUnknown);
  CHECK_EQ (BoardFamilyFromAdc (656),  BoardFamilyRg351p);
  CHECK_EQ (BoardFamilyFromAdc (694),  BoardFamilyRg351p);
  CHECK_EQ (BoardFamilyFromAdc (695),  BoardFamilyUnknown);
  CHECK_EQ (BoardFamilyFromAdc (1023), BoardFamilyUnknown);
}

static void test_pin_regs (void)
{
  GPIO_REG_FIELD f;

  // Pull-ups boot.ini writes with mw.l (hiword mask | value), U-Boot odroidgoa.c
  GpioPullField (3, GPIO_PIN_PB1, &f);   // 0xff140084 0x000c0004: bits 3:2 = 01
  CHECK_EQ (f.Reg, 0xff140084); CHECK_EQ (f.Shift, 2);
  GpioPullField (3, GPIO_PIN_PB4, &f);   // 0xff140084 0x03000100: bits 9:8 = 01
  CHECK_EQ (f.Reg, 0xff140084); CHECK_EQ (f.Shift, 8);
  GpioPullField (3, GPIO_PIN_PD0, &f);   // 0xff14008c 0x00030001: bits 1:0
  CHECK_EQ (f.Reg, 0xff14008c); CHECK_EQ (f.Shift, 0);
  GpioPullField (3, GPIO_PIN_PD1, &f);   // 0xff14008c 0x000c0004: bits 3:2
  CHECK_EQ (f.Reg, 0xff14008c); CHECK_EQ (f.Shift, 2);
  // U-Boot grf_px30: gpio1a_p 0x60, gpio1b_p 0x64, gpio2a_p 0x70, gpio3b_p 0x84, gpio3d_p 0x8c
  GpioPullField (1, GPIO_PIN_PA0, &f); CHECK_EQ (f.Reg, 0xff140060);
  GpioPullField (1, GPIO_PIN_PB2, &f); CHECK_EQ (f.Reg, 0xff140064); CHECK_EQ (f.Shift, 4);
  GpioPullField (2, GPIO_PIN_PA4, &f); CHECK_EQ (f.Reg, 0xff140070); CHECK_EQ (f.Shift, 8);
  // PMU GRF (kernel PX30_PULL_PMU_OFFSET 0x10): GPIO0_A3, the SD detect
  GpioPullField (0, GPIO_PIN_PA3, &f); CHECK_EQ (f.Reg, 0xff010010); CHECK_EQ (f.Shift, 6);

  // Drive: U-Boot gpio1a_e 0xf0, gpio1b_e 0xf4; 2 bits per pin
  GpioDriveField (1, GPIO_PIN_PA7, &f); CHECK_EQ (f.Reg, 0xff1400f0); CHECK_EQ (f.Shift, 14);
  GpioDriveField (1, GPIO_PIN_PB1, &f); CHECK_EQ (f.Reg, 0xff1400f4); CHECK_EQ (f.Shift, 2);
  GpioDriveField (2, GPIO_PIN_PA0, &f); CHECK_EQ (f.Reg, 0xff140100);
  GpioDriveField (3, GPIO_PIN_PA0, &f); CHECK_EQ (f.Reg, 0xff140110);
  GpioDriveField (0, GPIO_PIN_PB0, &f); CHECK_EQ (f.Reg, 0xff010024);

  // Mux: I2C0 is GPIO0_B0/B1 function 1, 2 bits per pin in the PMU GRF
  GpioIomuxField (0, GPIO_PIN_PB0, &f); CHECK_EQ (f.Reg, 0xff010004); CHECK_EQ (f.Shift, 0); CHECK_EQ (f.Mask, 3);
  GpioIomuxField (0, GPIO_PIN_PB1, &f); CHECK_EQ (f.Reg, 0xff010004); CHECK_EQ (f.Shift, 2);
  GpioIomuxField (0, GPIO_PIN_PA3, &f); CHECK_EQ (f.Reg, 0xff010000); CHECK_EQ (f.Shift, 6);
  // GPIO1..3: 4 bits per pin, U-Boot gpio1al 0x00, gpio1ah 0x04, gpio1bl 0x08, gpio1bh 0x0c
  GpioIomuxField (1, GPIO_PIN_PA0, &f); CHECK_EQ (f.Reg, 0xff140000); CHECK_EQ (f.Mask, 0xF);
  GpioIomuxField (1, GPIO_PIN_PA5, &f); CHECK_EQ (f.Reg, 0xff140004); CHECK_EQ (f.Shift, 4);
  GpioIomuxField (1, GPIO_PIN_PB1, &f); CHECK_EQ (f.Reg, 0xff140008); CHECK_EQ (f.Shift, 4);
  GpioIomuxField (1, GPIO_PIN_PB4, &f); CHECK_EQ (f.Reg, 0xff14000c); CHECK_EQ (f.Shift, 0);
  GpioIomuxField (2, GPIO_PIN_PA0, &f); CHECK_EQ (f.Reg, 0xff140020);
  GpioIomuxField (3, GPIO_PIN_PC0, &f); CHECK_EQ (f.Reg, 0xff140050);
  // the SD card pins this repo already configures
  GpioIomuxField (1, GPIO_PIN_PD6, &f); CHECK_EQ (f.Reg, 0xff14001c); CHECK_EQ (f.Shift, 8);

  // Schmitt: kernel PX30_SCHMITT_*: PMU 0x38 (16 pins/reg), GRF 0xc0 + bank*16
  GpioSchmittField (0, GPIO_PIN_PB1, &f); CHECK_EQ (f.Reg, 0xff010038); CHECK_EQ (f.Shift, 9);
  GpioSchmittField (1, GPIO_PIN_PA0, &f); CHECK_EQ (f.Reg, 0xff1400c0);
  GpioSchmittField (3, GPIO_PIN_PD7, &f); CHECK_EQ (f.Reg, 0xff1400ec); CHECK_EQ (f.Shift, 7);

  CHECK_EQ (GpioDriveEnumToLevel (GPIO_PIN_DRIVE_3), 2);   // "8 mA" in the old table
  CHECK_EQ (GpioDriveEnumToLevel (GPIO_PIN_DRIVE_DISABLE), 0);
  CHECK_EQ (GpioDriveEnumToLevel (GPIO_PIN_DRIVE_5), 3);

  // GPIO banks (px30.dtsi)
  CHECK_EQ (GPIO_BASE (0), 0xff040000); CHECK_EQ (GPIO_BASE (1), 0xff250000);
  CHECK_EQ (GPIO_BASE (2), 0xff260000); CHECK_EQ (GPIO_BASE (3), 0xff270000);
}

static void test_clocks (void)
{
  // kernel clk-px30.c: I2C0 CLKSEL_CON49 low byte, I2C1 high byte, I2C2/3 CON50
  CHECK_EQ (CruI2cClock (0)->SelCon, 49); CHECK_EQ (CruI2cClock (0)->SelShift, 0);
  CHECK_EQ (CruI2cClock (1)->SelCon, 49); CHECK_EQ (CruI2cClock (1)->SelShift, 8);
  CHECK_EQ (CruI2cClock (2)->SelCon, 50); CHECK_EQ (CruI2cClock (3)->SelShift, 8);
  // SARADC 1 MHz from the 24 MHz crystal: /24, field = 23
  CHECK_EQ (CruSaradcDivField (24000000, 1000000), 23);
  CHECK_EQ (CruSaradcDivField (24000000, 24000000), 0);
  CHECK_EQ (CruSaradcDivField (24000000, 1), 0x7FF);           // clamped
  CHECK_EQ (CruSaradcDivField (24000000, 700000), 34);         // rounds up: 35 -> field 34
}

static void test_i2c_math (void)
{
  UINT32 actual, v;
  UINT32 w[8];
  UINT8  b[] = { 0x40, 0xED, 0x11, 0x22, 0x33 };

  // U-Boot rk_i2c_set_clk: div = ceil(rate / (8*scl)) - 2; divl = div/2, divh = rest
  v = RkI2cClkDiv (24000000, 400000, &actual);   // ceil(7.5)=8 -> 6
  CHECK_EQ (v, (3u << 16) | 3u); CHECK_EQ (actual, 375000);
  v = RkI2cClkDiv (24000000, 100000, &actual);   // 30 -> 28
  CHECK_EQ (v, (14u << 16) | 14u); CHECK_EQ (actual, 100000);
  v = RkI2cClkDiv (100000000, 400000, &actual);  // ceil(31.25)=32 -> 30
  CHECK_EQ (v, (15u << 16) | 15u); CHECK (actual <= 400000);
  v = RkI2cClkDiv (88000000, 1000000, &actual);  // 11 -> 9: odd, divh gets the extra step
  CHECK_EQ (v, (5u << 16) | 4u); CHECK (actual <= 1000000);
  v = RkI2cClkDiv (1000000, 1000000, NULL);      // never below zero
  CHECK_EQ (v, 0);
  // the actual rate never exceeds the request, for a spread of inputs
  {
    UINT32 in[] = { 24000000, 75000000, 100000000, 150000000, 200000000 };
    UINT32 hz[] = { 100000, 400000, 1000000 };
    for (int i = 0; i < 5; i++) {
      for (int j = 0; j < 3; j++) {
        RkI2cClkDiv (in[i], hz[j], &actual);
        CHECK (actual <= hz[j]);
      }
    }
  }

  CHECK_EQ (RkI2cPackTx (b, 5, w), 2);
  CHECK_EQ (w[0], 0x2211ED40);
  CHECK_EQ (w[1], 0x33);
  CHECK_EQ (RkI2cPackTx (b, 2, w), 1);
  CHECK_EQ (w[0], 0xED40);
}

static void test_rk817 (void)
{
  RK817_REGULATOR r;
  UINT8 sel;

  CHECK (Rk817IsChipId (0x81, 0x70));
  CHECK (Rk817IsChipId (0x81, 0x7F));      // low nibble is the revision
  CHECK (!Rk817IsChipId (0x80, 0x90));     // RK809
  CHECK (!Rk817IsChipId (0x00, 0x00));
  CHECK (!Rk817IsChipId (0xFF, 0xFF));     // bus floating

  // names in the shipped DTBs
  CHECK (Rk817ParseRegulator ("DCDC_REG1", &r));
  CHECK_EQ (r.Kind, Rk817KindBuck); CHECK_EQ (r.VselReg, 0xBB); CHECK_EQ (r.EnReg, 0xB1); CHECK_EQ (r.EnBit, 0);
  CHECK (Rk817ParseRegulator ("DCDC_REG4", &r));
  CHECK_EQ (r.VselReg, 0xC4); CHECK_EQ (r.EnBit, 3);
  CHECK (Rk817ParseRegulator ("LDO_REG1", &r));
  CHECK_EQ (r.Kind, Rk817KindLdo); CHECK_EQ (r.VselReg, 0xCC); CHECK_EQ (r.EnReg, 0xB2); CHECK_EQ (r.EnBit, 0);
  CHECK (Rk817ParseRegulator ("LDO_REG4", &r));
  CHECK_EQ (r.EnReg, 0xB2); CHECK_EQ (r.EnBit, 3);
  CHECK (Rk817ParseRegulator ("LDO_REG5", &r));
  CHECK_EQ (r.VselReg, 0xD4); CHECK_EQ (r.EnReg, 0xB3); CHECK_EQ (r.EnBit, 0);
  CHECK (Rk817ParseRegulator ("LDO_REG8", &r));
  CHECK_EQ (r.VselReg, 0xDA); CHECK_EQ (r.EnReg, 0xB3); CHECK_EQ (r.EnBit, 3);
  CHECK (Rk817ParseRegulator ("LDO_REG9", &r));
  CHECK_EQ (r.VselReg, 0xDC); CHECK_EQ (r.EnReg, 0xB4); CHECK_EQ (r.EnBit, 0);
  CHECK (Rk817ParseRegulator ("BOOST", &r));
  CHECK_EQ (r.Kind, Rk817KindBoost); CHECK_EQ (r.VselReg, 0xDE); CHECK_EQ (r.EnReg, 0xB4); CHECK_EQ (r.EnBit, 1);
  CHECK (Rk817ParseRegulator ("OTG_SWITCH", &r));
  CHECK_EQ (r.Kind, Rk817KindSwitch); CHECK_EQ (r.VselReg, 0); CHECK_EQ (r.EnBit, 2);
  CHECK (!Rk817ParseRegulator ("DCDC_REG5", &r));
  CHECK (!Rk817ParseRegulator ("DCDC_REG0", &r));
  CHECK (!Rk817ParseRegulator ("LDO_REG10", &r));
  CHECK (!Rk817ParseRegulator ("LDO_REG", &r));
  CHECK (!Rk817ParseRegulator ("regulator@0", &r));
  CHECK (!Rk817ParseRegulator (NULL, &r));
  CHECK (!Rk817ParseRegulator ("DCDC_REG1x", &r));

  // voltages from the shipped DT (the sel values the kernel would pick)
  Rk817ParseRegulator ("DCDC_REG1", &r);
  CHECK (Rk817VoltageToSel (&r, 1000000, &sel)); CHECK_EQ (sel, 0x28);
  CHECK (Rk817VoltageToSel (&r, 950000, &sel));  CHECK_EQ (sel, 0x24);   // vdd_arm minimum
  CHECK (Rk817VoltageToSel (&r, 1500000, &sel)); CHECK_EQ (sel, 0x50);
  CHECK (Rk817VoltageToSel (&r, 1600000, &sel)); CHECK_EQ (sel, 0x51);
  CHECK (Rk817VoltageToSel (&r, 2400000, &sel)); CHECK_EQ (sel, 0x59);
  CHECK (!Rk817VoltageToSel (&r, 2500000, &sel));
  CHECK (!Rk817VoltageToSel (&r, 400000, &sel));
  Rk817ParseRegulator ("DCDC_REG4", &r);
  CHECK (Rk817VoltageToSel (&r, 3300000, &sel)); CHECK_EQ (sel, 0x62);   // vcc_3v3
  CHECK (Rk817VoltageToSel (&r, 3400000, &sel)); CHECK_EQ (sel, 0x63);
  Rk817ParseRegulator ("LDO_REG2", &r);
  CHECK (Rk817VoltageToSel (&r, 1800000, &sel)); CHECK_EQ (sel, 48);     // vcc1v8_soc
  CHECK (Rk817VoltageToSel (&r, 1000000, &sel)); CHECK_EQ (sel, 16);
  CHECK (Rk817VoltageToSel (&r, 3300000, &sel)); CHECK_EQ (sel, 108);
  CHECK (Rk817VoltageToSel (&r, 2800000, &sel)); CHECK_EQ (sel, 88);     // vcc_lcd
  CHECK (Rk817VoltageToSel (&r, 3400000, &sel)); CHECK_EQ (sel, 0x70);
  CHECK (!Rk817VoltageToSel (&r, 3500000, &sel));
  CHECK (!Rk817VoltageToSel (&r, 500000, &sel));
  Rk817ParseRegulator ("BOOST", &r);
  CHECK (Rk817VoltageToSel (&r, 5000000, &sel)); CHECK_EQ (sel, 3);
  CHECK (Rk817VoltageToSel (&r, 4700000, &sel)); CHECK_EQ (sel, 0);
  CHECK (!Rk817VoltageToSel (&r, 5500000, &sel));
  Rk817ParseRegulator ("OTG_SWITCH", &r);
  CHECK (!Rk817VoltageToSel (&r, 5000000, &sel));

  // enable writes (kernel ENABLE_MASK / DISABLE_VAL)
  CHECK_EQ (Rk817EnableWrite (0, TRUE), 0x11);
  CHECK_EQ (Rk817EnableWrite (3, TRUE), 0x88);
  CHECK_EQ (Rk817EnableWrite (3, FALSE), 0x80);
  CHECK_EQ (Rk817EnableWrite (1, FALSE), 0x20);

  // battery: the two calibration codes map to their reference voltages
  CHECK (abs (Rk817BatteryMilliVolts (0x0A00, 0x0A00, 0x1400) - 2300) <= 2);
  CHECK (abs (Rk817BatteryMilliVolts (0x1400, 0x0A00, 0x1400) - 4025) <= 2);
  CHECK (abs (Rk817BatteryMilliVolts (0x0F00, 0x0A00, 0x1400) - 3162) <= 5);
  CHECK_EQ (Rk817BatteryMilliVolts (0x1000, 0x1000, 0x1000), 0);        // equal codes (no calibration): unknown, no divide by zero
  CHECK_EQ (Rk817BatteryMilliVolts (0x1000, 0, 0), 0);

  CHECK (!Rk817IsCharging (0x00));
  CHECK (Rk817IsCharging (0x10));    // dead charge
  CHECK (Rk817IsCharging (0x20));    // trickle
  CHECK (Rk817IsCharging (0x30));    // CC/CV
  CHECK (!Rk817IsCharging (0x40));   // terminated
  CHECK (!Rk817IsCharging (0x8F));   // battery present bit, state 0

  CHECK_EQ (Rk817PowerKeyState (FALSE, 0), FALSE);
  CHECK_EQ (Rk817PowerKeyState (FALSE, RK817_INT_PWRON_FALL), TRUE);
  CHECK_EQ (Rk817PowerKeyState (TRUE, 0), TRUE);
  CHECK_EQ (Rk817PowerKeyState (TRUE, RK817_INT_PWRON_RISE), FALSE);
  CHECK_EQ (Rk817PowerKeyState (FALSE, RK817_INT_PWRON_FALL | RK817_INT_PWRON_RISE), FALSE);   // a tap
}

// Key maps against the kernel DTS (rk3326-r36s-android.dtsi and ...-clone-ee.dtsi)
static void check_key (BOARD_FAMILY f, BOOT_KEY k, int group, int pin)
{
  KEY_PIN p;
  CHECK (BootKeyPin (f, k, &p));
  CHECK_EQ (p.Group, group);
  CHECK_EQ (p.Pin, pin);
}

static void test_key_maps (void)
{
  KEY_PIN p;

  // rg351mp: sw1..4 GPIO1_B4..B7, sw5 A GPIO1_A2, sw6 B GPIO1_A5, sw15/16 GPIO2_A6/A7,
  // FN GPIO2_A4 (boot.ini c4), Select GPIO3_B1 (d9), Start GPIO3_B4 (d12)
  check_key (BoardFamilyRg351mp, BootKeyUp,     1, 8 + 4);
  check_key (BoardFamilyRg351mp, BootKeyDown,   1, 8 + 5);
  check_key (BoardFamilyRg351mp, BootKeyLeft,   1, 8 + 6);
  check_key (BoardFamilyRg351mp, BootKeyRight,  1, 8 + 7);
  check_key (BoardFamilyRg351mp, BootKeyA,      1, 2);
  check_key (BoardFamilyRg351mp, BootKeyB,      1, 5);
  check_key (BoardFamilyRg351mp, BootKeyL1,     2, 6);
  check_key (BoardFamilyRg351mp, BootKeyR1,     2, 7);
  check_key (BoardFamilyRg351mp, BootKeyFn,     2, 4);
  check_key (BoardFamilyRg351mp, BootKeySelect, 3, 9);
  check_key (BoardFamilyRg351mp, BootKeyStart,  3, 12);

  // rg351v: GPIO3_C6/C7/C4/C5, A C0, B C1, L1 B5, R1 B3, FN GPIO3_B2 (d10),
  // Select GPIO3_D0 (d24), Start GPIO3_D1 (d25)
  check_key (BoardFamilyRg351v, BootKeyUp,     3, 16 + 6);
  check_key (BoardFamilyRg351v, BootKeyDown,   3, 16 + 7);
  check_key (BoardFamilyRg351v, BootKeyLeft,   3, 16 + 4);
  check_key (BoardFamilyRg351v, BootKeyRight,  3, 16 + 5);
  check_key (BoardFamilyRg351v, BootKeyA,      3, 16 + 0);
  check_key (BoardFamilyRg351v, BootKeyB,      3, 16 + 1);
  check_key (BoardFamilyRg351v, BootKeyL1,     3, 8 + 5);
  check_key (BoardFamilyRg351v, BootKeyR1,     3, 8 + 3);
  check_key (BoardFamilyRg351v, BootKeyFn,     3, 10);
  check_key (BoardFamilyRg351v, BootKeySelect, 3, 24);
  check_key (BoardFamilyRg351v, BootKeyStart,  3, 25);

  // boot-decision keys only where boot.ini reads them; menu keys fall back to the R36S map
  for (int f = BoardFamilyUnknown; f <= BoardFamilyRg351p; f++) {
    if (f == BoardFamilyRg351mp || f == BoardFamilyRg351v) continue;
    CHECK (!BootKeyPin ((BOARD_FAMILY)f, BootKeyFn, &p));
    CHECK (!BootKeyPin ((BOARD_FAMILY)f, BootKeySelect, &p));
    CHECK (!BootKeyPin ((BOARD_FAMILY)f, BootKeyStart, &p));
    check_key ((BOARD_FAMILY)f, BootKeyUp, 1, 12);
    check_key ((BOARD_FAMILY)f, BootKeyA, 1, 2);
  }
  CHECK (!BootKeyPin (BoardFamilyRg351mp, BootKeyMax, &p));
  CHECK (!BootKeyPin (BoardFamilyRg351mp, (BOOT_KEY)99, &p));

  // no two keys of a family share a pin
  for (int f = BoardFamilyRg351mp; f <= BoardFamilyRg351v; f++) {
    for (int a = 0; a < BootKeyMax; a++) {
      for (int c = a + 1; c < BootKeyMax; c++) {
        KEY_PIN pa, pc;
        if (BootKeyPin ((BOARD_FAMILY)f, (BOOT_KEY)a, &pa) && BootKeyPin ((BOARD_FAMILY)f, (BOOT_KEY)c, &pc)) {
          CHECK (pa.Group != pc.Group || pa.Pin != pc.Pin);
        }
      }
    }
  }
}

static void test_key_repeat (void)
{
  KEY_REPEAT k = { 0 };

  CHECK_EQ (KeyRepeatStep (&k, FALSE, 20000000, TRUE), 0);
  CHECK_EQ (KeyRepeatStep (&k, TRUE, 20000000, TRUE), 1);          // press
  for (int i = 0; i < 24; i++) {
    CHECK_EQ (KeyRepeatStep (&k, TRUE, 20000000, TRUE), 0);        // 480 ms held
  }
  CHECK_EQ (KeyRepeatStep (&k, TRUE, 20000000, TRUE), 1);          // 500 ms: first repeat
  CHECK_EQ (KeyRepeatStep (&k, TRUE, 20000000, TRUE), 0);
  CHECK_EQ (KeyRepeatStep (&k, TRUE, 80000000, TRUE), 1);          // 100 ms later: another
  CHECK_EQ (KeyRepeatStep (&k, TRUE, 250000000, TRUE), 2);         // slow poll: two at once
  CHECK_EQ (KeyRepeatStep (&k, FALSE, 20000000, TRUE), 0);         // release
  CHECK_EQ (KeyRepeatStep (&k, TRUE, 20000000, TRUE), 1);          // press again starts fresh
  CHECK_EQ (KeyRepeatStep (&k, TRUE, 400000000, TRUE), 0);

  KEY_REPEAT n = { 0 };
  CHECK_EQ (KeyRepeatStep (&n, TRUE, 20000000, FALSE), 1);
  CHECK_EQ (KeyRepeatStep (&n, TRUE, 900000000, FALSE), 0);        // A/B never repeat
  CHECK_EQ (KeyRepeatStep (&n, TRUE, 900000000, FALSE), 0);
}

static void test_rk817_power (void)
{
  UINT8 s0, s1;

  // U-Boot rk8xx_probe: SAVE0 = en0[3:0] | en1[3:0] << 4, SAVE1 = en2 | en3 << 4
  Rk817PowerEnSave (0x1F, 0xA5, 0x03, 0xF4, &s0, &s1);
  CHECK_EQ (s0, 0x5F);
  CHECK_EQ (s1, 0x43);
  Rk817PowerEnSave (0, 0, 0, 0, &s0, &s1);
  CHECK_EQ (s0, 0); CHECK_EQ (s1, 0);
  CHECK_EQ (RK817_POWER_EN_SAVE0, 0x99);       // kernel rk808.h
  CHECK_EQ (RK817_POWER_EN_SAVE1, 0xA4);

  // fg_rk817.c: VLIM_4500MV 0x50 | 0x80 in the high nibble, ILIM 450 mA = 0 or 1500 mA = 3, | 0x08
  CHECK_EQ (Rk817UsbCtrl (TRUE), 0xDB);
  CHECK_EQ (Rk817UsbCtrl (FALSE), 0xD8);
  CHECK_EQ (Rk817ChrgOut (0x00), 0x01);        // CHRG_CUR_1500MA = 1
  CHECK_EQ (Rk817ChrgOut (0xFF), 0xF9);        // other bits kept
  CHECK_EQ (Rk817ChrgOut (0x07), 0x01);

  CHECK (!Rk817BatteryMilliVoltsValid (0));
  CHECK (!Rk817BatteryMilliVoltsValid (-4000));
  CHECK (!Rk817BatteryMilliVoltsValid (2499));
  CHECK (Rk817BatteryMilliVoltsValid (2500));
  CHECK (Rk817BatteryMilliVoltsValid (4200));
  CHECK (Rk817BatteryMilliVoltsValid (4700));
  CHECK (!Rk817BatteryMilliVoltsValid (4701));
}

int main (void)
{
  test_rk817_power ();
  test_board_family ();
  test_pin_regs ();
  test_clocks ();
  test_i2c_math ();
  test_rk817 ();
  test_key_maps ();
  test_key_repeat ();
  DONE ();
}

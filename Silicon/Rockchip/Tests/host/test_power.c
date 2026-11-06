// PowerPolicy.h: the start decision as a table of states -> action, the OCV
// percentage, and the charge screen state machine driven through scripted
// scenarios. Thresholds are U-Boot's (odroidgoa_power.c, the screen DTB's
// charge-animation node: 3530, 3090, 3055+50, 10 s, 5 s), written out here.
#include <Uefi.h>
#include "test.h"
#include "PowerPolicy.h"

typedef struct {
  const char *name;
  POWER_INPUTS in;
  POWER_START want;
} CASE;

#define ST(ok, mv, bat, pin, plug, mode)  { ok, mv, bat, pin, plug, mode }

static void test_start (void)
{
  static const CASE cases[] = {
    // name                       ok  mV    bat  pin  plug mode
    { "good battery, no charger",   ST (1, 3900, 1, 0, 0, 0),           PowerStartContinue },
    { "exactly 3530",               ST (1, 3530, 1, 0, 0, 0),           PowerStartContinue },
    { "3529 no charger",            ST (1, 3529, 1, 0, 0, 0),           PowerStartLowBattery },
    { "very low no charger",        ST (1, 3000, 1, 0, 0, 0),           PowerStartLowBattery },
    { "low, dc pin only",           ST (1, 3300, 1, 1, 0, 0),           PowerStartContinue },
    { "low, pmic plug only",        ST (1, 3300, 1, 0, 1, 0),           PowerStartContinue },
    { "low, both, cold start",      ST (1, 3300, 1, 1, 1, 0),           PowerStartCharge },
    { "good, both, cold start",     ST (1, 3900, 1, 1, 1, 0),           PowerStartCharge },
    { "good, both, reboot (NORMAL)",ST (1, 3900, 1, 1, 1, 0x5242C300),  PowerStartContinue },
    { "both, other reboot reason",  ST (1, 3900, 1, 1, 1, 0x5242C303),  PowerStartCharge },
    { "both, no battery",           ST (1, 0,    0, 1, 1, 0),           PowerStartContinue },
    { "voltage unknown, no charger",ST (1, 0,    1, 0, 0, 0),           PowerStartContinue },
    { "pmic dead",                  ST (0, 3000, 0, 0, 0, 0),           PowerStartContinue },
    { "pin but no plug",            ST (1, 3900, 1, 1, 0, 0),           PowerStartContinue },
  };

  for (unsigned i = 0; i < sizeof (cases) / sizeof (cases[0]); i++) {
    POWER_START got = PowerStartDecide (&cases[i].in);
    if (got != cases[i].want) printf ("  case '%s'\n", cases[i].name);
    CHECK_EQ (got, cases[i].want);
  }
}

static void test_percent (void)
{
  // OCV table points of the screen DTB's battery node, 5 % per point
  CHECK_EQ (PowerPercentFromMv (0), 0);
  CHECK_EQ (PowerPercentFromMv (3165), 0);
  CHECK_EQ (PowerPercentFromMv (3220), 5);
  CHECK_EQ (PowerPercentFromMv (3530), 35);
  CHECK_EQ (PowerPercentFromMv (3951), 85);
  CHECK_EQ (PowerPercentFromMv (4000), 100);
  CHECK_EQ (PowerPercentFromMv (4300), 100);
  CHECK_EQ (PowerPercentFromMv (3505), 32);     // halfway 3480..3530 is 32.5 -> 32
  unsigned prev = 0;
  for (unsigned mv = 3000; mv < 4300; mv++) {   // never goes backwards, stays within 0..100
    unsigned p = PowerPercentFromMv (mv);
    CHECK (p >= prev && p <= 100);
    prev = p;
  }
}

// One scripted tick: time in ms, battery mV, plug, key down.
typedef struct { UINT64 t; UINT32 mv; int plug; int key; int tap; POWER_ACTION act; int redraw; int screen; } STEP;

static void run (const char *name, const STEP *s, int n)
{
  POWER_CHARGE_STATE st = {0};
  POWER_CHARGE_OUT out;
  for (int i = 0; i < n; i++) {
    PowerChargeStep (&st, s[i].t, s[i].mv, s[i].plug, s[i].key, s[i].tap, &out);
    if (out.Action != s[i].act || (s[i].redraw >= 0 && out.Redraw != s[i].redraw) || (s[i].screen >= 0 && st.ScreenOn != s[i].screen)) {
      printf ("  scenario '%s' step %d: action %d (want %d) redraw %d (want %d) screen %d (want %d)\n",
              name, i, out.Action, s[i].act, out.Redraw, s[i].redraw, st.ScreenOn, s[i].screen);
    }
    CHECK_EQ (out.Action, s[i].act);
    if (s[i].redraw >= 0) CHECK_EQ (out.Redraw, s[i].redraw);
    if (s[i].screen >= 0) CHECK_EQ (st.ScreenOn, s[i].screen);
  }
}

#define N PowerActNone
#define OFF PowerActPowerOff
#define GO PowerActContinueBoot

static void test_charge (void)
{
  // 1. Normal charge, screen off after 10 s, a short press (release seen) wakes it, a tap again turns it off
  {
    STEP s[] = {
      {     0, 3800, 1, 0, 0, N, 1, 1 },
      {  9950, 3800, 1, 0, 0, N, 0, 1 },
      { 10000, 3800, 1, 0, 0, N, 1, 0 },     // 10 s: off
      { 12000, 3800, 1, 1, 0, N, 0, 0 },     // key down
      { 12100, 3800, 1, 0, 0, N, 1, 1 },     // released after 100 ms: screen on
      { 12150, 3800, 1, 0, 1, N, 1, 0 },     // PMIC tap: screen off again
      { 12200, 3800, 1, 0, 1, N, 1, 1 },
    };
    run ("timeout and short press", s, 7);
  }
  // 2. Screen on again restarts the 10 s timer from the wake
  {
    STEP s[] = {
      {     0, 3800, 1, 0, 0, N, 1, 1 },
      { 10000, 3800, 1, 0, 0, N, 1, 0 },
      { 20000, 3800, 1, 0, 1, N, 1, 1 },     // tap wakes
      { 29999, 3800, 1, 0, 0, N, 0, 1 },
      { 30000, 3800, 1, 0, 0, N, 1, 0 },
    };
    run ("timer restarts", s, 5);
  }
  // 3. Long press with enough battery continues the boot (2 s hold), screen off or on
  {
    STEP s[] = {
      {     0, 3800, 1, 0, 0, N, 1, 1 },
      { 10000, 3800, 1, 0, 0, N, 1, 0 },
      { 11000, 3800, 1, 1, 0, N, 0, 0 },
      { 12999, 3800, 1, 1, 0, N, 0, 0 },
      { 13000, 3800, 1, 1, 0, GO, -1, 1 },
    };
    run ("long press", s, 5);
  }
  // 4. Boundary: 3105 mV is normal, 3104 is extreme low (keys ignored, screen off after 5 s)
  {
    STEP s[] = {
      {    0, 3105, 1, 0, 0, N, 1, 1 },      // 3105 is not extreme
      { 1000, 3105, 1, 1, 0, N, 0, 1 },
      { 3000, 3105, 1, 1, 0, GO, -1, 1 },
    };
    run ("3105 boots on long press", s, 3);
  }
  {
    STEP s[] = {
      {    0, 3104, 1, 0, 0, N, 1, 1 },      // extreme low
      { 1000, 3104, 1, 1, 0, N, 0, 1 },
      { 3500, 3104, 1, 1, 0, N, 0, 1 },      // keys ignored
      { 5000, 3104, 1, 0, 0, N, 1, 0 },      // screen off after 5 s
      { 6000, 3104, 1, 1, 0, N, 0, 0 },
      { 9000, 3104, 1, 1, 0, N, 0, 0 },      // still ignored, still off
      { 9100, 3104, 1, 0, 1, N, 0, 0 },      // a tap does not wake it
      { 9200, 3110, 1, 0, 0, N, 1, 1 },      // above the threshold: screen back on
    };
    run ("extreme low", s, 8);
  }
  // 6. Charger removed: power off at once, in every state
  {
    STEP s[] = {
      {    0, 3800, 1, 0, 0, N,   1, 1 },
      { 5000, 3800, 0, 0, 0, OFF, -1, -1 },
    };
    run ("unplugged", s, 2);
    STEP t[] = {
      {    0, 3000, 1, 0, 0, N,   1, 1 },
      { 6000, 3000, 0, 0, 0, OFF, -1, -1 },
    };
    run ("unplugged extreme", t, 2);
  }
  // 6b. Unknown voltage (0) behaves as normal
  {
    STEP s[] = {
      {    0, 0, 1, 0, 0, N, 1, 1 },
      { 1000, 0, 1, 1, 0, N, 0, 1 },
      { 3000, 0, 1, 1, 0, GO, -1, 1 },
    };
    run ("unknown voltage boots", s, 3);
  }
  // 8. Key held across the 10 s screen-off: no tap on release after a long press that booted/was handled
  {
    POWER_CHARGE_STATE st = {0};
    POWER_CHARGE_OUT out;
    PowerChargeStep (&st, 0, 3800, 1, 0, 0, &out);
    PowerChargeStep (&st, 100, 3800, 1, 1, 0, &out);
    PowerChargeStep (&st, 2200, 3800, 1, 1, 0, &out);      // long press -> boot
    CHECK_EQ (out.Action, GO);
  }
}

int main (void)
{
  test_start ();
  test_percent ();
  test_charge ();
  DONE ();
}

// BootKeysLib.c and KeypadDeviceLib.c with mocked pads and SARADC. One process
// per board id because the library reads the ADC once and remembers it.
//   ./test_keys 166   R36S-like    ./test_keys 514   EE clone
//   ./test_keys 300   unknown      ./test_keys fail  SARADC error
#include <Uefi.h>
#include <Library/BootKeysLib.h>
#include <Library/CruLib.h>
#include <Library/GpioLib.h>
#include <Library/RkSaradcLib.h>
#include <Library/KeypadDeviceLib.h>
#include <string.h>
#include "test.h"

static int  level[4][32];            // 1 = high
static int  pulled[4][32], fn0[4][32], dir_in[4][32];
static int  adc_value, adc_fail, adc_reads, other_writes;

EFI_STATUS EFIAPI RkSaradcRead (UINT8 Ch, UINT32 *V)
{
  adc_reads++;
  CHECK_EQ (Ch, 0);
  if (adc_fail) return EFI_TIMEOUT;
  *V = adc_value;
  return EFI_SUCCESS;
}

VOID CruEnableGpioClocks (VOID) {}
BOOLEAN GpioPinGetLevel (UINT8 g, UINT8 p) { return level[g][p]; }
VOID GpioPinSetFunction (UINT8 g, UINT8 p, UINT8 f) { fn0[g][p] = (f == 0); if (f) other_writes++; }
VOID GpioPinSetDirection (UINT8 g, UINT8 p, GPIO_PIN_DIRECTION d) { dir_in[g][p] = (d == GPIO_PIN_INPUT); }
VOID GpioPinSetPull (UINT8 g, UINT8 p, GPIO_PIN_PULL pull) { pulled[g][p] = (pull == GPIO_PIN_PULL_UP); }

static int events[64];
static int nev;
static VOID EFIAPI Push (KEYPAD_RETURN_API *T, EFI_KEY_DATA *K)
{
  events[nev++] = K->Key.ScanCode ? K->Key.ScanCode : K->Key.UnicodeChar;
}

static void press (int g, int p, int down) { level[g][p] = !down; }

static int configured_pins (void)
{
  int n = 0;
  for (int g = 0; g < 4; g++) for (int p = 0; p < 32; p++) if (pulled[g][p]) n++;
  return n;
}

int main (int argc, char **argv)
{
  KEYPAD_RETURN_API api = { Push };

  memset (level, 1, sizeof (level));       // everything released (pulled up)
  for (int g = 0; g < 4; g++) for (int p = 0; p < 32; p++) level[g][p] = 1;

  if (!strcmp (argv[1], "fail")) {
    adc_fail = 1;
  } else {
    adc_value = atoi (argv[1]);
  }

  if (adc_value == 166) {
    CHECK_EQ (BootKeysGetBoardFamily (), BoardFamilyRg351mp);
    CHECK_EQ (adc_reads, 1);
    CHECK_EQ (BootKeysGetBoardFamily (), BoardFamilyRg351mp);
    CHECK_EQ (adc_reads, 1);               // cached
    CHECK_EQ (configured_pins (), 11);     // all eleven keys got a pull-up
    CHECK (pulled[3][9] && pulled[3][12] && pulled[2][4] && pulled[1][12] && pulled[1][2]);
    CHECK (fn0[3][9] && dir_in[3][9]);     // GPIO function, input
    CHECK_EQ (other_writes, 0);

    for (int k = 0; k < BootKeyMax; k++) CHECK (!BootKeyPressed ((BOOT_KEY)k));
    press (3, 9, 1);                       // Select = GPIO3_B1
    CHECK (BootKeyPressed (BootKeySelect));
    CHECK (!BootKeyPressed (BootKeyStart));
    CHECK (!BootKeyPressed (BootKeyFn));
    press (3, 9, 0);
    press (2, 4, 1);                       // FN = GPIO2_A4
    CHECK (BootKeyPressed (BootKeyFn));
    CHECK (!BootKeyPressed (BootKeySelect));
    press (2, 4, 0);
    press (3, 12, 1);                      // Start = GPIO3_B4
    CHECK (BootKeyPressed (BootKeyStart));
    press (3, 12, 0);
    CHECK (!BootKeyPressed (BootKeyMax));

    // text input: press, hold, release
    press (1, 12, 1);                      // Up
    nev = 0;
    KeypadDeviceReset (NULL);
    KeypadDeviceGetKeys (NULL, &api, 20000000);
    CHECK_EQ (nev, 1); CHECK_EQ (events[0], SCAN_UP);
    for (int i = 0; i < 24; i++) KeypadDeviceGetKeys (NULL, &api, 20000000);
    CHECK_EQ (nev, 1);                     // 480 ms: no repeat yet
    KeypadDeviceGetKeys (NULL, &api, 20000000);
    CHECK_EQ (nev, 2);                     // repeat starts at 500 ms
    press (1, 12, 0);
    KeypadDeviceGetKeys (NULL, &api, 20000000);
    CHECK_EQ (nev, 2);
    press (1, 2, 1);                       // A: Enter, once
    press (1, 5, 1);                       // B: Escape
    KeypadDeviceGetKeys (NULL, &api, 20000000);
    CHECK_EQ (nev, 4);
    CHECK ((events[2] == CHAR_CARRIAGE_RETURN && events[3] == SCAN_ESC) || (events[3] == CHAR_CARRIAGE_RETURN && events[2] == SCAN_ESC));
    for (int i = 0; i < 40; i++) KeypadDeviceGetKeys (NULL, &api, 20000000);
    CHECK_EQ (nev, 4);                     // A and B do not repeat
    press (1, 2, 0); press (1, 5, 0);
    // keys with no text meaning (Select/Start/FN) produce nothing
    press (3, 9, 1); press (3, 12, 1); press (2, 4, 1);
    nev = 0;
    KeypadDeviceGetKeys (NULL, &api, 20000000);
    CHECK_EQ (nev, 0);
    // all eight text keys
    press (3, 9, 0); press (3, 12, 0); press (2, 4, 0);
    KeypadDeviceReset (NULL);
    KeypadDeviceGetKeys (NULL, &api, 20000000);
    press (1, 13, 1); press (1, 14, 1); press (1, 15, 1); press (2, 6, 1); press (2, 7, 1);
    nev = 0;
    KeypadDeviceGetKeys (NULL, &api, 20000000);
    CHECK_EQ (nev, 5);
  } else if (adc_value == 514) {
    CHECK_EQ (BootKeysGetBoardFamily (), BoardFamilyRg351v);
    CHECK_EQ (configured_pins (), 11);
    CHECK (pulled[3][10] && pulled[3][24] && pulled[3][25]);
    CHECK (!pulled[1][12]);                // the R36S d-pad pins are not touched
    press (3, 10, 1);                      // FN = GPIO3_B2
    CHECK (BootKeyPressed (BootKeyFn));
    press (3, 10, 0);
    press (3, 24, 1);                      // Select = GPIO3_D0
    press (3, 25, 1);                      // Start = GPIO3_D1
    CHECK (BootKeyPressed (BootKeySelect) && BootKeyPressed (BootKeyStart));
    CHECK (!BootKeyPressed (BootKeyFn));
    press (1, 12, 1);                      // R36S "Up" pin means nothing here
    CHECK (!BootKeyPressed (BootKeyUp));
    press (3, 22, 1);                      // GPIO3_C6
    CHECK (BootKeyPressed (BootKeyUp));
  } else {
    // unknown family or failed ADC: no boot-decision keys, menu keys as R36S
    CHECK_EQ (BootKeysGetBoardFamily (), BoardFamilyUnknown);
    CHECK_EQ (configured_pins (), 8);
    press (2, 4, 1); press (3, 9, 1); press (3, 12, 1);
    CHECK (!BootKeyPressed (BootKeyFn));
    CHECK (!BootKeyPressed (BootKeySelect));
    CHECK (!BootKeyPressed (BootKeyStart));
    press (1, 12, 1);
    CHECK (BootKeyPressed (BootKeyUp));
  }
  DONE ();
}

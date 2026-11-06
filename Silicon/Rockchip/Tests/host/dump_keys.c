// Prints the key table for check-keys-vs-dts.py.
#include <Uefi.h>
#include <Library/GpioLib.h>
#include <Library/BootKeysLib.h>
#include "BootKeysMap.h"
#include <stdio.h>

int main (void)
{
  static const char *names[] = { "fn", "select", "start", "up", "down", "left", "right", "a", "b", "l1", "r1" };
  KEY_PIN p;

  for (int f = BoardFamilyRg351mp; f <= BoardFamilyRg351v; f++) {
    for (int k = 0; k < BootKeyMax; k++) {
      if (BootKeyPin ((BOARD_FAMILY)f, (BOOT_KEY)k, &p)) {
        printf ("%s %s %d %d\n", f == BoardFamilyRg351mp ? "mp" : "v", names[k], p.Group, p.Pin);
      }
    }
  }
  return 0;
}

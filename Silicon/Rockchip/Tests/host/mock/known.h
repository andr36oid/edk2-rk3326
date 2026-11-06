// Firmware bugs a test has found but that are not fixed yet. A KNOWN check
// prints the failing input every run without failing the suite, so the other
// suites stay usable; STRICT_KNOWN=1 in the environment makes them fail.
// Include after test.h.
#include <stdio.h>
#include <stdlib.h>

static int g_known;

#define KNOWN(cond, id, ...) do { g_checks++; if (!(cond)) { g_known++; \
  printf ("KNOWN BUG %s (%s:%d): %s: ", id, __FILE__, __LINE__, #cond); printf (__VA_ARGS__); printf ("\n"); \
  if (getenv ("STRICT_KNOWN") != NULL) { g_fails++; } } } while (0)

#define DONE_KNOWN() do { printf ("%d checks, %d failed, %d known-bug hits\n", g_checks, g_fails, g_known); \
  return g_fails ? 1 : 0; } while (0)

// Tiny assertion helpers for the host tests.
#include <stdio.h>
#include <stdlib.h>

static int g_checks, g_fails;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fails++; printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; \
  if (_a != _b) { g_fails++; printf ("FAIL %s:%d: %s == %s  (%lld vs %lld, 0x%llx vs 0x%llx)\n", __FILE__, __LINE__, #a, #b, _a, _b, _a, _b); } } while (0)
#define DONE() do { printf ("%d checks, %d failed\n", g_checks, g_fails); return g_fails ? 1 : 0; } while (0)

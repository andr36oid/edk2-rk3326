// SPDX-License-Identifier: BSD-2-Clause-Patent

#ifndef FAKE_ENV_H_
#define FAKE_ENV_H_

#define FAKE_TRACE_MAX  200000

typedef struct {
  UINTN     Addr;
  UINT32    Value;     // as written
  UINT32    After;     // register content afterwards (write masks applied)
  UINT64    Time;      // fake microseconds
} FAKE_WRITE;

typedef struct {
  const char    *Name;
  BOOLEAN       Enable;
  int           Trace;  // fake_trace_count at the call
} FAKE_PMIC_CALL;

extern int             fake_verbose;
extern int             fake_errors;
extern int             fake_warnings;
extern UINT64          fake_time_us;
extern FAKE_WRITE      fake_trace[FAKE_TRACE_MAX];
extern int             fake_trace_count;
extern FAKE_PMIC_CALL  fake_pmic[64];
extern int             fake_pmic_count;

UINT32  fake_get (UINTN Addr);
void    fake_set (UINTN Addr, UINT32 Val);
void    fake_reset (void);

#endif

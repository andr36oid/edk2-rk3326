/*
  The test "kernel": checks what the boot app hands over against
  Documentation/arm64/booting.rst, prints the command line and the initrd on
  the PL011 and powers off through PSCI. Runs with the MMU off: everything is
  Device memory, so no unaligned accesses (-mstrict-align, bytewise FDT reads).
*/

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef unsigned char  u8;

#define PL011        0x09000000UL
#define TEXT_OFFSET  0x80000UL
#define IMAGE_SIZE   0x400000UL
#define SZ_2M        0x200000UL
#define LOG_PREV     0x41E80000UL      // qemuvirt's RAM log, the previous start's half

#define PSCI_SYSTEM_OFF    0x84000008UL
#define PSCI_SYSTEM_RESET  0x84000009UL

extern u64 psci_call (u64 fn, u64 smc);

static void
putc_ (char c)
{
  volatile u32  *dr = (volatile u32 *)PL011;
  volatile u32  *fr = (volatile u32 *)(PL011 + 0x18);

  while (*fr & (1 << 5)) {
  }

  *dr = (u8)c;
}

static void
puts_ (const char *s)
{
  while (*s) {
    if (*s == '\n') {
      putc_ ('\r');
    }

    putc_ (*s++);
  }
}

static void
puthex (u64 v)
{
  int  i;

  puts_ ("0x");
  for (i = 60; i >= 0; i -= 4) {
    putc_ ("0123456789abcdef"[(v >> i) & 15]);
  }
}

static int  fails;

static void
check (int ok, const char *what)
{
  puts_ (ok ? "TESTKERNEL: ok   " : "TESTKERNEL: FAIL ");
  puts_ (what);
  puts_ ("\n");
  if (!ok) {
    fails++;
  }
}

static u64
be (const u8 *p, int n)
{
  u64  v = 0;

  while (n--) {
    v = (v << 8) | *p++;
  }

  return v;
}

static int
streq (const char *a, const char *b)
{
  while (*a && *a == *b) {
    a++;
    b++;
  }

  return *a == *b;
}

static int
prefix (const char *s, const char *p)
{
  while (*p) {
    if (*s++ != *p++) {
      return 0;
    }
  }

  return 1;
}

static int
contains (const char *s, const char *w)
{
  for ( ; *s; s++) {
    if (prefix (s, w)) {
      return 1;
    }
  }

  return 0;
}

struct fdt_info {
  const char    *bootargs;
  u64           initrd_start, initrd_end;
  int           have_initrd;
  u64           mem_base[4], mem_size[4];
  int           mem_count;
};

static void
parse_fdt (const u8 *fdt, struct fdt_info *fi)
{
  const u8    *st   = fdt + be (fdt + 8, 4);
  const char  *strs = (const char *)fdt + be (fdt + 12, 4);
  u64         off   = 0;
  int         depth = 0;
  int         in_chosen = 0, in_memory = 0;
  int         ac = 2, sc = 1;
  u32         tok, len, i;
  const char  *name;

  for ( ; ;) {
    tok  = be (st + off, 4);
    off += 4;
    if (tok == 1) {                       // BEGIN_NODE
      name = (const char *)st + off;
      for (i = 0; name[i]; i++) {
      }

      off = (off + i + 1 + 3) & ~3UL;
      depth++;
      if (depth == 2) {
        in_chosen = streq (name, "chosen");
        in_memory = prefix (name, "memory");
      }
    } else if (tok == 2) {                // END_NODE
      depth--;
      if (depth < 2) {
        in_chosen = in_memory = 0;
      }
    } else if (tok == 3) {                // PROP
      const u8  *v;

      len  = be (st + off, 4);
      name = strs + be (st + off + 4, 4);
      v    = st + off + 8;
      off  = (off + 8 + len + 3) & ~3UL;
      if (depth == 1) {
        if (streq (name, "#address-cells")) {
          ac = be (v, 4);
        } else if (streq (name, "#size-cells")) {
          sc = be (v, 4);
        }
      } else if ((depth == 2) && in_chosen) {
        if (streq (name, "bootargs")) {
          fi->bootargs = (const char *)v;
        } else if (streq (name, "linux,initrd-start")) {
          fi->initrd_start = be (v, len);
          fi->have_initrd |= 1;
        } else if (streq (name, "linux,initrd-end")) {
          fi->initrd_end   = be (v, len);
          fi->have_initrd |= 2;
        }
      } else if ((depth == 2) && in_memory && streq (name, "reg")) {
        for (i = 0; i + (ac + sc) * 4 <= len && fi->mem_count < 4; i += (ac + sc) * 4) {
          fi->mem_base[fi->mem_count] = be (v + i, ac * 4);
          fi->mem_size[fi->mem_count] = be (v + i + ac * 4, sc * 4);
          fi->mem_count++;
        }
      }
    } else if (tok == 4) {                // NOP
    } else {
      break;                              // END or junk
    }
  }
}

static int
in_memory (struct fdt_info *fi, u64 base, u64 size)
{
  int  i;

  for (i = 0; i < fi->mem_count; i++) {
    if ((base >= fi->mem_base[i]) && (base + size <= fi->mem_base[i] + fi->mem_size[i])) {
      return 1;
    }
  }

  return 0;
}

static int
overlaps (u64 a, u64 as, u64 b, u64 bs)
{
  return (a < b + bs) && (b < a + as);
}

void
test_main (u64 x0, u64 x1, u64 x2, u64 x3, u64 daif, u64 el, u64 sctlr, u64 entry)
{
  const u8         *fdt = (const u8 *)x0;
  struct fdt_info  fi;
  u64              fdt_size = 0, base = entry - TEXT_OFFSET;
  int              fdt_ok, i, want_reset, have_prev;
  const char       *p;

  fi.bootargs  = 0;
  fi.have_initrd = 0;
  fi.initrd_start = fi.initrd_end = 0;
  fi.mem_count = 0;

  puts_ ("\nTESTKERNEL: started at EL");
  putc_ ('0' + (char)el);
  puts_ (", entry ");
  puthex (entry);
  puts_ (", x0 ");
  puthex (x0);
  puts_ ("\n");

  check ((el == 1) || (el == 2), "entered at EL1 or EL2");
  check ((sctlr & 1) == 0, "MMU off");
  check ((sctlr & 4) == 0, "D-cache off");
  check ((daif & 0x3c0) == 0x3c0, "DAIF masked");
  check ((x1 | x2 | x3) == 0, "x1-x3 zero");
  check ((base & (SZ_2M - 1)) == 0, "Image at a 2 MB aligned base + text_offset");

  fdt_ok = ((x0 & 7) == 0) && (x0 != 0) && (be (fdt, 4) == 0xd00dfeedUL);
  check (fdt_ok, "x0 is an FDT (8-byte aligned, magic)");
  if (!fdt_ok) {
    goto Out;
  }

  fdt_size = be (fdt + 4, 4);
  check (fdt_size <= SZ_2M, "FDT at most 2 MB");
  check ((x0 / SZ_2M) == ((x0 + fdt_size - 1) / SZ_2M), "FDT inside one 2 MB block");
  check (!overlaps (x0, fdt_size, base, IMAGE_SIZE + TEXT_OFFSET), "FDT clear of the Image and its image_size");

  parse_fdt (fdt, &fi);
  puts_ ("TESTKERNEL: bootargs=");
  puts_ (fi.bootargs ? fi.bootargs : "(none)");
  puts_ ("\n");
  for (i = 0; i < fi.mem_count; i++) {
    puts_ ("TESTKERNEL: memory ");
    puthex (fi.mem_base[i]);
    puts_ ("+");
    puthex (fi.mem_size[i]);
    puts_ ("\n");
  }

  check (fi.mem_count > 0, "/memory present");
  check (in_memory (&fi, base, TEXT_OFFSET + IMAGE_SIZE), "Image inside /memory");
  check (in_memory (&fi, x0, fdt_size), "FDT inside /memory");

  if (fi.have_initrd) {
    const char  *rd = (const char *)fi.initrd_start;
    u64         n   = fi.initrd_end - fi.initrd_start;

    puts_ ("TESTKERNEL: initrd ");
    puthex (fi.initrd_start);
    puts_ ("-");
    puthex (fi.initrd_end);
    puts_ ("\n");
    check (fi.have_initrd == 3 && fi.initrd_end > fi.initrd_start, "initrd start and end");
    check (in_memory (&fi, fi.initrd_start, n), "initrd inside /memory");
    check (!overlaps (fi.initrd_start, n, base, IMAGE_SIZE + TEXT_OFFSET), "initrd clear of the Image and its image_size");
    check (!overlaps (fi.initrd_start, n, x0, fdt_size), "initrd clear of the FDT");
    check (prefix (rd, "ANDR36OID-TEST-INITRD:"), "initrd starts with the payload (uImage header gone)");
    check ((n > 11) && prefix (rd + n - 11, "INITRD-END\n"), "initrd ends where its size says");
    if (prefix (rd, "ANDR36OID-TEST-INITRD:")) {
      puts_ ("TESTKERNEL: initrd name=");
      for (p = rd + 22; *p && *p != '\n'; p++) {
        putc_ (*p);
      }

      puts_ ("\n");
    }
  } else {
    puts_ ("TESTKERNEL: no initrd\n");
  }

Out:
  puts_ (fails ? "TESTKERNEL: RESULT FAIL\n" : "TESTKERNEL: RESULT PASS\n");

  // A warm restart once, so the next start has a previous log (uefi-log-prev.txt)
  // (volatile: compared against a string literal GCC would fold the fixed
  // address into a link-time constant, and this image runs where it lands)
  {
    volatile const u8  *m = (volatile const u8 *)LOG_PREV;

    have_prev = (m[0] == 'Q') && (m[1] == 'V') && (m[2] == 'L') && (m[3] == 'G');
  }

  want_reset = fi.bootargs && contains (fi.bootargs, "qemu_test.reset_once");
  if (want_reset && !have_prev) {
    puts_ ("TESTKERNEL: warm restart\n");
    psci_call (PSCI_SYSTEM_RESET, el == 2);
  }

  puts_ ("TESTKERNEL: power off\n");
  psci_call (PSCI_SYSTEM_OFF, el == 2);
}

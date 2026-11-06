// RkPlatformInfoLib against U-Boot's own code (uboot_ref_mem.c):
//  - the miniloader's tags at 2 MB - 8 KB, built by U-Boot's atags_set_tag
//    (js_hash included) and read back by both atags_get_tag and the library,
//    plus truncated, corrupt, oversized and odd tags;
//  - the DDR record in PMUGRF OS_REG2/3, written by U-Boot's encoder
//    (sdram_org_config) for every geometry, its true size from
//    sdram_get_cs_cap, U-Boot's decoder rockchip_sdram_size, and random words;
//  - the VOP window U-Boot's logo code programs, and broken VOP states.
// The tag area is mapped at its real address with no-access pages around it,
// so a read outside the 8 KB crashes the test.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent

#define _GNU_SOURCE
#include <sys/mman.h>
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/RkPlatformInfoLib.h>
#include <Soc/Rk3326.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "known.h"
#include "uboot_ref_mem.h"

// ---- register model ------------------------------------------------------------

static UINT32       os_reg2, os_reg3;
static UINT32       pmu_pwrdn_st, pmu_bus_idle_st;   // PMU_PWRDN_ST 0xFF000020, PMU_BUS_IDLE_ST 0xFF00006C
static UB_VOP_REGS  vopb, vopl;

static UINT32 *VopReg (UB_VOP_REGS *V, UINTN Off)
{
  switch (Off) {
    case 0x018: return &V->sys_ctrl2;
    case 0x090: return &V->win1_ctrl0;
    case 0x098: return &V->win1_vir;
    case 0x0A0: return &V->win1_mst;
    case 0x0A4: return &V->win1_dsp_info;
    case 0x104: return &V->hact_st_end;
    case 0x10C: return &V->vact_st_end;
  }

  return NULL;
}

UINT32 EFIAPI MmioRead32 (UINTN a)
{
  UINT32  *R = NULL;

  if (a == 0xFF010208UL) return os_reg2;     // PMUGRF OS_REG2 (U-Boot get_ddr_os_reg: 0xff010208)
  if (a == 0xFF01020CUL) return os_reg3;
  if (a == 0xFF000020UL) return pmu_pwrdn_st;
  if (a == 0xFF00006CUL) return pmu_bus_idle_st;
  if ((a & ~0xFFFFUL) == 0xFF460000UL) R = VopReg (&vopb, a & 0xFFFF);
  if ((a & ~0xFFFFUL) == 0xFF470000UL) R = VopReg (&vopl, a & 0xFFFF);
  if (R == NULL) {
    printf ("FAIL: read of an unexpected register 0x%lx\n", (unsigned long)a);
    exit (1);
  }

  return *R;
}

UINT32 EFIAPI MmioWrite32 (UINTN a, UINT32 v)
{
  // The library is documented as read-only (safe from SEC, before anything is set up).
  printf ("FAIL: RkPlatformInfoLib wrote 0x%x to 0x%lx\n", v, (unsigned long)a);
  exit (1);
}

UINT64 EFIAPI LShiftU64 (IN UINT64 Operand, IN UINTN Count) { return Operand << Count; }

// ---- DDR record -------------------------------------------------------------------

static UINT64 Ours (UINT32 R2, UINT32 R3)
{
  os_reg2 = R2;
  os_reg3 = R3;
  return RkGetDramSize ();
}

static int  n_valid, n_small, n_cap, n_known_cap;

// One geometry through U-Boot's encoder; the size must match the capacity
// U-Boot's DDR driver computes and what U-Boot's decoder reads back.
static void OneGeometry (UINT32 Type, struct ub_sdram_cap_info *Cap, BOOLEAN Legacy)
{
  struct ub_sdram_base_params  Base = { 333, Type, 1, 0, 0 };
  UINT32                       R2 = 0, R3 = 0;
  UINT64                       Truth, Capped, Ub, Got;

  ub_sdram_org_config (Cap, &Base, &R2, &R3, 0);
  if (Legacy) {
    R3 = 0;     // DDR blobs before the version-2 record: OS_REG3 unused
  }

  Truth = ub_sdram_get_cs_cap (Cap, 2, Type);
  if (Cap->row_3_4) {
    Truth = Truth * 3 / 4;
  }

  Capped = Truth > UB_SDRAM_MAX_SIZE ? UB_SDRAM_MAX_SIZE : Truth;
  Ub     = ub_rockchip_sdram_size (R2, R3);
  Got    = Ours (R2, R3);

  // The two U-Boot halves agree with each other.
  CHECK_EQ (Ub, Capped);

  if (Truth < SIZE_128MB) {
    // Below anything a PX30 console has: either reading is acceptable.
    n_small++;
    CHECK ((Got == 0) || (Got == Truth));
  } else if (Truth <= UB_SDRAM_MAX_SIZE) {
    n_valid++;
    if (Got != Truth) {
      printf ("FAIL: type %u rank %u col %u bk %u bw %u dbw %u r34 %u rows %u/%u%s: 0x%x/0x%x -> 0x%llx, want 0x%llx\n",
              Type, Cap->rank, Cap->col, Cap->bk, Cap->bw, Cap->dbw, Cap->row_3_4, Cap->cs0_row, Cap->cs1_row,
              Legacy ? " legacy" : "", R2, R3, (unsigned long long)Got, (unsigned long long)Truth);
    }

    CHECK_EQ (Got, Truth);
  } else {
    // 4 GB and more: U-Boot clamps to the RAM window below the registers
    // (SDRAM_MAX_SIZE 0xff000000). RAM must never reach 0xFF000000.
    n_cap++;
    CHECK ((Got == 0) || (Got <= UB_SDRAM_MAX_SIZE) || (Got == SIZE_4GB));
    CHECK ((Got == 0) || (Got <= UB_SDRAM_MAX_SIZE));
  }
}

static void TestDramEncoder (void)
{
  static CONST UINT32  Types[] = { UB_DDR3, UB_LPDDR3, UB_DDR4, UB_LPDDR2, UB_LPDDR4, UB_DDR2 };
  struct ub_sdram_cap_info  C;
  UINT32  t, rank, col, bk, bw, dbw, r34, row0, row1;
  int     Before = g_checks;

  for (t = 0; t < ARRAY_SIZE (Types); t++)
  for (rank = 1; rank <= 2; rank++)
  for (col = 9; col <= 12; col++)
  for (bk = 2; bk <= 3; bk++)
  for (bw = 0; bw <= 2; bw++)
  for (dbw = 0; dbw <= 2; dbw++)
  for (r34 = 0; r34 <= 1; r34++)
  for (row0 = 12; row0 <= 19; row0++)
  for (row1 = (rank == 2 ? 12 : 0); row1 <= (rank == 2 ? row0 : 0); row1++) {
    if ((Types[t] != UB_DDR4) && (dbw != bw)) {
      continue;   // the die width only matters for DDR4's bank groups
    }

    memset (&C, 0, sizeof (C));
    C.rank = rank; C.col = col; C.bk = bk; C.bw = bw; C.dbw = dbw; C.row_3_4 = r34;
    C.cs0_row = row0; C.cs1_row = row1;
    OneGeometry (Types[t], &C, FALSE);
    if ((row0 >= 13) && (row0 <= 16) && ((rank == 1) || (row1 >= 13))) {
      OneGeometry (Types[t], &C, TRUE);
    }
  }

  printf ("DDR record: %d geometry checks (%d in 128 MB..4080 MB, %d smaller, %d at 4 GB or more)\n",
          g_checks - Before, n_valid, n_small, n_cap);
}

// Records consoles actually have, by name (each also in the sweep).
static void TestDramNamed (void)
{
  static CONST struct { CONST char *Name; UINT32 Type, Rank, Col, Bk, Bw, R34, Row0, Row1; UINT64 Mb; } K[] = {
    { "512 MB DDR3 x16",           UB_DDR3,   1, 10, 3, 1, 0, 15,  0,  512 },
    { "512 MB LPDDR3 x32",         UB_LPDDR3, 1, 10, 3, 2, 0, 14,  0,  512 },
    { "1 GB DDR3 x32",             UB_DDR3,   1, 10, 3, 2, 0, 15,  0, 1024 },
    { "1 GB DDR3 two ranks",       UB_DDR3,   2, 10, 3, 2, 0, 14, 14, 1024 },
    { "768 MB odd ranks",          UB_DDR3,   2, 10, 3, 2, 0, 14, 13,  768 },
    { "1.5 GB (3/4 rows)",         UB_LPDDR3, 1, 10, 3, 2, 1, 16,  0, 1536 },
    { "2 GB LPDDR3",               UB_LPDDR3, 2, 10, 3, 2, 0, 15, 15, 2048 },
    { "3 GB (3/4 rows, 2 ranks)",  UB_LPDDR3, 2, 10, 3, 2, 1, 16, 16, 3072 },
    { "256 MB",                    UB_DDR3,   1, 10, 3, 1, 0, 14,  0,  256 },
    { "128 MB",                    UB_DDR3,   1, 10, 3, 1, 0, 13,  0,  128 },
  };
  UINTN  i;

  for (i = 0; i < ARRAY_SIZE (K); i++) {
    struct ub_sdram_cap_info     C = { 0 };
    struct ub_sdram_base_params  B = { 333, K[i].Type, 1, 0, 0 };
    UINT32                       R2 = 0, R3 = 0;

    C.rank = K[i].Rank; C.col = K[i].Col; C.bk = K[i].Bk; C.bw = K[i].Bw; C.dbw = K[i].Bw;
    C.row_3_4 = K[i].R34; C.cs0_row = K[i].Row0; C.cs1_row = K[i].Row1;
    ub_sdram_org_config (&C, &B, &R2, &R3, 0);
    CHECK_EQ (ub_rockchip_sdram_size (R2, R3) >> 20, K[i].Mb);
    CHECK_EQ (Ours (R2, R3) >> 20, K[i].Mb);
    printf ("  %-26s OS_REG2 0x%08x OS_REG3 0x%08x -> %llu MB\n", K[i].Name, R2, R3,
            (unsigned long long)(Ours (R2, R3) >> 20));
  }

  // Nothing recorded: the registers' reset value. A real record always
  // carries the channel-info bit (SYS_REG_ENC_CHINFO), so all zeros means
  // "missing", which the header says reads as 0.
  CHECK (Ours (0, 0) == 0);
  // All ones claims two channels, which the PX30 does not have; U-Boot
  // still decodes it, and so may the library, but never differently.
  CHECK ((Ours (0xFFFFFFFF, 0xFFFFFFFF) == 0) || (Ours (0xFFFFFFFF, 0xFFFFFFFF) == ub_rockchip_sdram_size (0xFFFFFFFF, 0xFFFFFFFF)));
}

static UINT64  rng = 0x9E3779B97F4A7C15ULL;
static UINT32 Rand (void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (UINT32)(rng >> 16); }

// Random words: wherever U-Boot's decoder is well defined, the library either
// agrees with it or (outside 128 MB..4 GB) says it does not know.
static void TestDramRandom (void)
{
  int  i, Compared = 0;

  for (i = 0; i < 400000; i++) {
    UINT32  R2 = Rand () ^ (Rand () << 16), R3 = Rand () ^ (Rand () << 16);
    UINT32  Ch, ChNum = 1 + ((R2 >> 12) & 1);
    BOOLEAN Defined = TRUE;
    UINT64  Ub, Got;

    if (i & 1) {
      R3 = (R3 & 0x0FFFFFFF) | (2U << 28);    // half of them as version-2 records
    }

    // U-Boot shifts an int by (rows + cols + banks + bw - 20) and by the
    // rank difference: only compare where both shifts are defined.
    for (Ch = 0; Ch < ChNum; Ch++) {
      UINT32  V2 = ((R3 >> 28) & 0xF) == 2;
      UINT32  Col0 = 9 + ((R2 >> (9 + Ch * 16)) & 3), Col1 = V2 ? 9 + ((R3 >> (Ch * 2)) & 3) : Col0;
      UINT32  E0 = V2 ? ((R3 >> (5 + Ch * 2)) & 1) << 2 : 0, E1 = V2 ? ((R3 >> (4 + Ch * 2)) & 1) << 2 : 0;
      UINT32  F0 = (R2 >> (6 + Ch * 16)) & 3, F1 = (R2 >> (4 + Ch * 16)) & 3;
      UINT32  Row0 = (V2 && (E0 + F0 == 7)) ? 12 : 13 + F0 + E0, Row1 = (V2 && (E1 + F1 == 7)) ? 12 : 13 + F1 + E1;
      UINT32  Bk = 3 - ((R2 >> (8 + Ch * 16)) & 1), Bw = 2 >> ((R2 >> (2 + Ch * 16)) & 3);
      UINT32  Bg = (((R2 >> 13) & 7) == 0) ? ((((R2 >> (Ch * 16)) & 3) == 2) ? 2 : 1) : 0;
      INT32   Sum = (INT32)(Row0 + Col0 + Bk + Bg + Bw) - 20;
      INT32   Shift = (INT32)(Row0 - Row1) + (INT32)(Col0 - Col1);
      UINT32  Rank = 1 + ((R2 >> (11 + Ch * 16)) & 1);

      if ((Sum < 0) || (Sum > 30) || ((Rank > 1) && ((Shift < 0) || (Shift > 63)))) {
        Defined = FALSE;
      }
    }

    if (!Defined) {
      continue;
    }

    Compared++;
    Ub  = ub_rockchip_sdram_size (R2, R3);
    Got = Ours (R2, R3);
    if (Ub < SIZE_128MB) {
      CHECK ((Got == 0) || (Got == Ub));
    } else if (Ub < UB_SDRAM_MAX_SIZE) {
      if (Got != Ub) {
        printf ("FAIL: random record 0x%08x/0x%08x: ours 0x%llx, U-Boot 0x%llx\n", R2, R3,
                (unsigned long long)Got, (unsigned long long)Ub);
      }

      CHECK_EQ (Got, Ub);
    } else {
      CHECK ((Got == 0) || (Got == UB_SDRAM_MAX_SIZE) || (Got == SIZE_4GB));   // 4 GB: see dram-4g
    }
  }

  printf ("DDR record: %d random words compared with U-Boot's decoder\n", Compared);
}

// ---- miniloader tags ----------------------------------------------------------------

#define TAGS  ((UINT8 *)(UINTN)UB_ATAGS_PHYS_BASE)

static void ClearTags (void) { memset (TAGS, 0, UB_ATAGS_SIZE); }

// The tag of that type in the list (header), via U-Boot's walk ignoring hashes.
static UINT32 *RawTag (UINT32 Magic)
{
  UINT32  *T = (UINT32 *)TAGS;

  while ((UINT8 *)T < TAGS + UB_ATAGS_SIZE && T[0] != 0) {
    if (T[1] == Magic) {
      return T;
    }

    T += T[0];
  }

  return NULL;
}

static void Rehash (UINT32 *T)
{
  T[T[0] - 1] = ub_js_hash (T, T[0] * 4 - 4);
}

// What U-Boot makes of the current tags, as RK_BOOT_TAGS (OP-TEE as
// param_parse_optee_mem reserves it, boot device as param_parse_bootdev).
static void UbootView (RK_BOOT_TAGS *V)
{
  uint64_t  Base, Size;
  UINT32  Dev;

  memset (V, 0, sizeof (*V));
  if (ub_param_bootdev_type (&Dev) == 0) {
    struct ub_tag_bootdev  *B = (struct ub_tag_bootdev *)((UINT8 *)ub_atags_get_tag (UB_ATAG_BOOTDEV) + 8);

    V->BootDevType = Dev;
    V->BootDevNum  = B->devnum;
  }

  ub_param_parse_optee_mem (&Base, &Size);
  V->TeeBase = Base;
  V->TeeSize = (UINT32)Size;
}

static void AddBootdev (UINT32 Type, UINT32 Num)
{
  struct ub_tag_bootdev  B;

  memset (&B, 0, sizeof (B));
  B.version = 1;
  B.devtype = Type;
  B.devnum  = Num;
  B.mode    = 0;
  B.reserved[0] = 0xA5A5A5A5;    // high bytes in the hashed data (char signedness)
  CHECK_EQ (ub_atags_set_tag (UB_ATAG_BOOTDEV, &B), 0);
}

static void AddTos (UINT64 Base, UINT32 Size, UINT32 Flags)
{
  struct ub_tag_tos_mem  T;

  memset (&T, 0, sizeof (T));
  T.version = 1;
  memcpy (T.tee_mem.name, "op-tee", 6);
  T.tee_mem.phy_addr = Base;
  T.tee_mem.size     = Size;
  T.tee_mem.flags    = Flags;
  memcpy (T.drm_mem.name, "drm", 3);
  T.reserved[0] = 0xFFFFFFFFFFFFFFFFULL;
  CHECK_EQ (ub_atags_set_tag (UB_ATAG_TOS_MEM, &T), 0);
}

static void AddOthers (void)
{
  struct ub_tag_serial    S = { 0 };
  struct ub_tag_ddr_mem   D = { 0 };
  struct ub_tag_atf_mem   A = { 0 };
  struct ub_tag_soc_info  I = { 0 };

  S.version = 1; S.enable = 1; S.addr = 0xFF160000; S.baudrate = 115200; S.m_mode = 1; S.id = 2;
  D.count = 1; D.version = 1; D.bank[0] = 0x200000; D.bank[10] = 0x3FE00000;
  A.version = 1; A.phy_addr = 0x10000; A.size = 0xF0000;
  I.version = 1; I.name = 0x3326; I.flags = 0x45543030;
  CHECK_EQ (ub_atags_set_tag (UB_ATAG_SERIAL, &S), 0);
  CHECK_EQ (ub_atags_set_tag (UB_ATAG_DDR_MEM, &D), 0);
  CHECK_EQ (ub_atags_set_tag (UB_ATAG_ATF_MEM, &A), 0);
  CHECK_EQ (ub_atags_set_tag (UB_ATAG_SOC_INFO, &I), 0);
}

static void ExpectTags (CONST char *What, EFI_STATUS WantStatus, UINT32 Dev, UINT32 Num, UINT64 TeeBase, UINT32 TeeSize)
{
  RK_BOOT_TAGS  T;
  EFI_STATUS    S;

  memset (&T, 0x5A, sizeof (T));
  S = RkGetBootTags (&T);
  if ((S != WantStatus) || (T.BootDevType != Dev) || (T.BootDevNum != Num) || (T.TeeBase != TeeBase) || (T.TeeSize != TeeSize)) {
    printf ("FAIL: tags '%s': status 0x%lx dev 0x%x/%u tee 0x%llx+0x%x; want 0x%lx dev 0x%x/%u tee 0x%llx+0x%x\n", What,
            (unsigned long)S, T.BootDevType, T.BootDevNum, (unsigned long long)T.TeeBase, T.TeeSize,
            (unsigned long)WantStatus, Dev, Num, (unsigned long long)TeeBase, TeeSize);
  }

  CHECK_EQ (S, WantStatus);
  CHECK_EQ (T.BootDevType, Dev);
  CHECK_EQ (T.BootDevNum, Num);
  CHECK_EQ (T.TeeBase, TeeBase);
  CHECK_EQ (T.TeeSize, TeeSize);
}

// Library and U-Boot agree on a list U-Boot accepts as it is.
static void ExpectLikeUboot (CONST char *What)
{
  RK_BOOT_TAGS  U;

  UbootView (&U);
  ExpectTags (What, EFI_SUCCESS, U.BootDevType, U.BootDevNum, U.TeeBase, U.TeeSize);
}

// The pre-ATAGs block at DDR + 34 MB, filled the way BL32 fills it.
static void SetTosParam (UINT64 Base, UINT32 Size, UINT32 Flags, BOOLEAN GoodSum)
{
  struct ub_tos_param_t  *P = (struct ub_tos_param_t *)(UINTN)UB_PARAM_OPTEE_INFO_OFFSET;

  memset (P, 0, sizeof (*P));
  P->version = 1;
  memcpy (P->tee_mem.name, "op-tee", 6);
  P->tee_mem.phy_addr = (int64_t)Base;
  P->tee_mem.size     = Size;
  P->tee_mem.flags    = Flags;
  P->reserve[3]       = 0x0123456789ABCDEFLL;
  P->checksum = ub_trust_checksum ((UINT8 *)P + 8, sizeof (*P) - 8) ^ (GoodSum ? 0 : 1);
}

static void ExpectBoth (CONST char *What, UINT64 Base, UINT32 Size)
{
  RK_BOOT_TAGS  R;
  uint64_t      UB, US;

  memset (&R, 0xAA, sizeof (R));
  RkGetBootTags (&R);
  ub_param_parse_optee_mem (&UB, &US);
  CHECK_EQ (R.TeeBase, Base);
  CHECK_EQ (R.TeeSize, Size);
  if ((R.TeeBase != Base) || (R.TeeSize != Size)) {
    printf ("    [%s] library 0x%llx+0x%x, U-Boot 0x%llx+0x%llx\n", What, (unsigned long long)R.TeeBase, R.TeeSize,
            (unsigned long long)UB, (unsigned long long)US);
  }
}

static void TestLegacyTos (void)
{
  // Tags present (boot device), no TOS tag: the legacy block counts.
  ClearTags ();
  AddBootdev (UB_BOOT_TYPE_SD0, 0);
  SetTosParam (0x08400000, 0x01E00000, 1, TRUE);
  ExpectBoth ("legacy block, as BL32 writes it", 0x08400000, 0x01E00000);
  SetTosParam (0x08400000, 0x01E00000, 1, FALSE);
  ExpectBoth ("legacy block, bad checksum", 0, 0);
  SetTosParam (0x08400000, 0x01E00000, 0, TRUE);
  ExpectBoth ("legacy block, flags 0", 0, 0);
  SetTosParam (0x08400000, 0x01E00800, 1, TRUE);
  ExpectBoth ("legacy block, size not a page multiple", 0, 0);
  // A TOS tag with flags 1 wins over the block.
  SetTosParam (0x08400000, 0x01E00000, 1, TRUE);
  AddTos (0x10000000, SIZE_16MB, 1);
  ExpectBoth ("tag over legacy block", 0x10000000, SIZE_16MB);
  // A TOS tag U-Boot ignores (flags 0) falls back to the block, as U-Boot does.
  ClearTags ();
  AddBootdev (UB_BOOT_TYPE_SD0, 0);
  AddTos (0x10000000, SIZE_16MB, 0);
  ExpectBoth ("tag flags 0, legacy block used", 0x08400000, 0x01E00000);
  memset ((void *)(UINTN)UB_PARAM_OPTEE_INFO_OFFSET, 0, sizeof (struct ub_tos_param_t));
}

static void TestTags (void)
{
  UINT32  *T;
  UINT32  Saved;

  // Nothing there (cold RAM, or U-Boot's atags_destroy before booti).
  ClearTags ();
  ExpectTags ("empty", EFI_NOT_FOUND, 0, 0, 0, 0);
  memset (TAGS, 0xFF, UB_ATAGS_SIZE);
  ExpectTags ("all ones", EFI_NOT_FOUND, 0, 0, 0, 0);

  // A full list the way the miniloader writes it: SD boot, OP-TEE at the
  // trust.img BL32 load address (0x08400000).
  ClearTags ();
  AddOthers ();
  AddBootdev (UB_BOOT_TYPE_SD0, 0);
  AddTos (0x08400000, SIZE_16MB, 1);
  ExpectLikeUboot ("SD, OP-TEE 16 MB");
  ExpectTags ("SD, OP-TEE 16 MB", EFI_SUCCESS, RK_BOOT_DEVICE_SD0, 0, 0x08400000, SIZE_16MB);

  // eMMC, OP-TEE first in the list, 32 MB.
  ClearTags ();
  AddTos (0x08400000, SIZE_32MB, 1);
  AddBootdev (UB_BOOT_TYPE_EMMC, 0);
  AddOthers ();
  ExpectLikeUboot ("eMMC, 32 MB");
  ExpectTags ("eMMC, 32 MB", EFI_SUCCESS, RK_BOOT_DEVICE_EMMC, 0, 0x08400000, SIZE_32MB);

  // Only the CORE tag + SERIAL (what the DDR init leaves): no fields.
  ClearTags ();
  {
    struct ub_tag_serial  S = { 0 };
    S.version = 1;
    CHECK_EQ (ub_atags_set_tag (UB_ATAG_SERIAL, &S), 0);
  }
  ExpectTags ("core+serial", EFI_SUCCESS, 0, 0, 0, 0);

  // U-Boot quiesce: atags_destroy() wipes the head of the list.
  ClearTags ();
  AddBootdev (UB_BOOT_TYPE_SD0, 0);
  AddTos (0x08400000, SIZE_16MB, 1);
  ub_atags_destroy ();
  ExpectTags ("destroyed", EFI_NOT_FOUND, 0, 0, 0, 0);

  // One corrupt byte in the boot device payload: U-Boot drops that tag, so
  // must we; the OP-TEE tag after it still counts.
  ClearTags ();
  AddBootdev (UB_BOOT_TYPE_EMMC, 0);
  AddTos (0x08400000, SIZE_16MB, 1);
  T = RawTag (UB_ATAG_BOOTDEV);
  T[3] ^= 0x40;
  CHECK (ub_atags_get_tag (UB_ATAG_BOOTDEV) == NULL);
  ExpectLikeUboot ("bootdev hash mismatch");
  ExpectTags ("bootdev hash mismatch", EFI_SUCCESS, 0, 0, 0x08400000, SIZE_16MB);

  // Corrupt hash word of the OP-TEE tag.
  ClearTags ();
  AddBootdev (UB_BOOT_TYPE_SD0, 1);
  AddTos (0x08400000, SIZE_16MB, 1);
  T = RawTag (UB_ATAG_TOS_MEM);
  T[T[0] - 1] += 1;
  ExpectLikeUboot ("tos hash mismatch");
  ExpectTags ("tos hash mismatch", EFI_SUCCESS, RK_BOOT_DEVICE_SD0, 1, 0, 0);

  // A tag that says it runs past the 8 KB: U-Boot stops (atags_overflow), and
  // the library must not read past 2 MB (the guard page would fault).
  ClearTags ();
  AddBootdev (UB_BOOT_TYPE_SD0, 0);
  AddTos (0x08400000, SIZE_16MB, 1);
  T = RawTag (UB_ATAG_BOOTDEV);
  Saved = T[0];
  T[0] = (UINT32)((TAGS + UB_ATAGS_SIZE - (UINT8 *)T) / 4) + 1;
  CHECK (ub_atags_get_tag (UB_ATAG_TOS_MEM) == NULL);
  ExpectTags ("oversized tag", EFI_SUCCESS, 0, 0, 0, 0);
  T[0] = Saved;

  // A size that wraps U-Boot's 32-bit (size << 2): 0x40000000 + n words.
  // U-Boot's overflow check sees n words; the library must refuse it.
  T[0] = 0x40000000U + Saved;
  ExpectTags ("size wraps 32 bits", EFI_SUCCESS, 0, 0, 0, 0);
  T[0] = 0xFFFFFFFFU;
  ExpectTags ("size 0xffffffff", EFI_SUCCESS, 0, 0, 0, 0);
  T[0] = Saved;
  ExpectTags ("restored", EFI_SUCCESS, RK_BOOT_DEVICE_SD0, 0, 0x08400000, SIZE_16MB);

  // Size 0 in the middle ends the list (U-Boot's for_each_tag); 1 and 2
  // words are too small to hold a hash.
  T = RawTag (UB_ATAG_BOOTDEV);
  Saved = T[0];
  T[0] = 0;
  ExpectLikeUboot ("size 0");
  ExpectTags ("size 0", EFI_SUCCESS, 0, 0, 0, 0);
  T[0] = 1;
  ExpectTags ("size 1", EFI_SUCCESS, 0, 0, 0, 0);
  T[0] = 2;
  ExpectTags ("size 2", EFI_SUCCESS, 0, 0, 0, 0);
  T[0] = Saved;

  // A boot device tag cut short (3 words: header + hash): no payload to read.
  ClearTags ();
  AddBootdev (UB_BOOT_TYPE_SD0, 0);
  T = RawTag (UB_ATAG_BOOTDEV);
  T[0] = 3;
  Rehash (T);
  T[3] = 0; T[4] = 0;   // what follows reads as the end of the list
  ExpectTags ("bootdev without payload", EFI_SUCCESS, 0, 0, 0, 0);

  // The last tag ends exactly at 2 MB.
  ClearTags ();
  {
    UINT32  *Core = (UINT32 *)TAGS;
    UINT32  Words = ub_tag_words (UB_ATAG_TOS_MEM);
    UINT32  *Last = (UINT32 *)(TAGS + UB_ATAGS_SIZE) - Words;
    struct ub_tag_tos_mem  *P = (struct ub_tag_tos_mem *)(Last + 2);

    Core[0] = (UINT32)(Last - Core);
    Core[1] = UB_ATAG_CORE;
    Last[0] = Words;
    Last[1] = UB_ATAG_TOS_MEM;
    P->tee_mem.phy_addr = 0x08400000;
    P->tee_mem.size     = SIZE_16MB;
    P->tee_mem.flags    = 1;
    Rehash (Last);
    // (U-Boot's own walk would read the word at 2 MB looking for a boot
    // device tag, so only its OP-TEE lookup is compared here.)
    CHECK (ub_atags_get_tag (UB_ATAG_TOS_MEM) == Last);
    ExpectTags ("tag ending at 2 MB", EFI_SUCCESS, 0, 0, 0x08400000, SIZE_16MB);
  }

  // An unknown magic before the tag: U-Boot gives up at it (bad_magic); the
  // library may skip it. Either way nothing wrong is read.
  ClearTags ();
  AddOthers ();
  AddBootdev (UB_BOOT_TYPE_SD1, 0);
  T = RawTag (UB_ATAG_SERIAL);
  T[1] = 0x12345678;
  CHECK (ub_atags_get_tag (UB_ATAG_BOOTDEV) == NULL);
  {
    RK_BOOT_TAGS  R;
    CHECK_EQ (RkGetBootTags (&R), EFI_SUCCESS);
    CHECK ((R.BootDevType == 0) || (R.BootDevType == RK_BOOT_DEVICE_SD1));
  }

  // OP-TEE regions that are not plausible are dropped.
  {
    static CONST struct { CONST char *Name; UINT64 Base; UINT32 Size; BOOLEAN Take; } O[] = {
      { "16 MB at 0x08400000",       0x08400000,          SIZE_16MB,  TRUE  },
      { "1 MB",                      0x08400000,          SIZE_1MB,   TRUE  },
      { "64 MB",                     0x08400000,          SIZE_64MB,  TRUE  },
      { "base 0",                    0,                   SIZE_16MB,  FALSE },
      { "size 0",                    0x08400000,          0,          FALSE },
      { "512 KB",                    0x08400000,          0x80000,    FALSE },
      { "65 MB",                     0x08400000,          SIZE_64MB + SIZE_1MB, FALSE },
      { "base not page aligned",     0x08400800,          SIZE_16MB,  FALSE },
      { "base above 4 GB",           0x108400000ULL,      SIZE_16MB,  FALSE },
      { "base at 4 GB - 16 MB",      0xFF000000ULL,       SIZE_16MB,  TRUE  },
    };
    UINTN  i;

    for (i = 0; i < ARRAY_SIZE (O); i++) {
      ClearTags ();
      AddTos (O[i].Base, O[i].Size, 1);
      ExpectTags (O[i].Name, EFI_SUCCESS, 0, 0, O[i].Take ? O[i].Base : 0, O[i].Take ? O[i].Size : 0);
    }
  }

  // Odd sizes: 1 MB + 2 KB passes the 1..64 MB rule, but the memory map can
  // only reserve whole 4 KB pages (ArmConfigureMmu rejects anything else).
  ClearTags ();
  AddTos (0x08400000, SIZE_1MB + 0x800, 1);
  {
    RK_BOOT_TAGS  R;
    RkGetBootTags (&R);
    CHECK (R.TeeSize == 0);
  }

  // U-Boot only takes OP-TEE when tee_mem.flags == 1 (param_parse_optee_mem).
  ClearTags ();
  AddTos (0x10000000, SIZE_32MB, 0);
  {
    RK_BOOT_TAGS  R, U;
    RkGetBootTags (&R);
    UbootView (&U);
    CHECK (R.TeeSize == U.TeeSize);
  }

  // A tag with hash 0 is accepted by U-Boot ("No hash"). The miniloader in
  // idbloader.img computes js_hash (seed 0x47C6A7E6 built with movz/movk at
  // offset 0x1b9cc), so the library's stricter rule costs nothing today.
  ClearTags ();
  AddBootdev (UB_BOOT_TYPE_SD0, 0);
  T = RawTag (UB_ATAG_BOOTDEV);
  T[T[0] - 1] = 0;
  CHECK (ub_atags_get_tag (UB_ATAG_BOOTDEV) != NULL);
  {
    RK_BOOT_TAGS  R;
    RkGetBootTags (&R);
    CHECK (R.BootDevType == UB_BOOT_TYPE_SD0);
  }

  // Every type the miniloader may write, then each read back: U-Boot's
  // list stays readable after the library looked at it (no writes).
  ClearTags ();
  AddOthers ();
  AddBootdev (UB_BOOT_TYPE_SD1, 3);
  AddTos (0x08400000, SIZE_16MB, 1);
  {
    UINT8  Before[UB_ATAGS_SIZE];
    RK_BOOT_TAGS  R;

    memcpy (Before, TAGS, UB_ATAGS_SIZE);
    RkGetBootTags (&R);
    CHECK (memcmp (Before, TAGS, UB_ATAGS_SIZE) == 0);
    CHECK_EQ (R.BootDevType, RK_BOOT_DEVICE_SD1);
    CHECK_EQ (R.BootDevNum, 3);
  }

  // Random damage anywhere in a good list: never a crash, and anything the
  // library returns is a value U-Boot would also read from an intact tag.
  {
    int  i;
    UINT8  Good[UB_ATAGS_SIZE];

    ClearTags ();
    AddOthers ();
    AddBootdev (UB_BOOT_TYPE_EMMC, 0);
    AddTos (0x08400000, SIZE_16MB, 1);
    memcpy (Good, TAGS, UB_ATAGS_SIZE);
    for (i = 0; i < 20000; i++) {
      RK_BOOT_TAGS  R;
      UINT32        Off = Rand () % 0x400;
      UINT8         Old = TAGS[Off];

      TAGS[Off] ^= (UINT8)(1 + Rand () % 255);
      RkGetBootTags (&R);
      CHECK ((R.BootDevType == 0) || (R.BootDevType == RK_BOOT_DEVICE_EMMC));
      CHECK ((R.TeeBase == 0) || ((R.TeeBase == 0x08400000) && (R.TeeSize == SIZE_16MB)));
      TAGS[Off] = Old;
    }

    CHECK (memcmp (Good, TAGS, UB_ATAGS_SIZE) == 0);
  }
}

// ---- VOP readback ---------------------------------------------------------------

// The R36S panel mode (rg351mp-uboot.dts: 640x480, hfp 119, hsync 2,
// hbp 119, vfp 13, vsync 2, vbp 5) and a 480x320 one.
static CONST UB_MODE  Mode640 = { 640, 640 + 119, 640 + 119 + 2, 640 + 119 + 2 + 119, 480, 480 + 13, 480 + 13 + 2, 480 + 13 + 2 + 5 };
static CONST UB_MODE  Mode480 = { 480, 480 + 40, 480 + 40 + 2, 480 + 40 + 2 + 40, 320, 320 + 8, 320 + 8 + 2, 320 + 8 + 2 + 4 };

static void VopReset (void)
{
  memset (&vopb, 0, sizeof (vopb));
  memset (&vopl, 0, sizeof (vopl));
  vopb.sys_ctrl2 = vopl.sys_ctrl2 = 1U << 1;   // standby after reset
}

static void ExpectFb (CONST char *What, BOOLEAN Found, UINT64 Base, UINT32 W, UINT32 H, UINT32 Stride, UINT32 Bpp, UINTN Vop)
{
  RK_FRAMEBUFFER_INFO  I;
  EFI_STATUS           S;

  memset (&I, 0, sizeof (I));
  S = RkGetActiveFramebuffer (&I);
  if (!Found) {
    if (!EFI_ERROR (S)) {
      printf ("FAIL: framebuffer '%s' reported: 0x%llx %ux%u stride %u %u bpp\n", What,
              (unsigned long long)I.Base, I.Width, I.Height, I.Stride, I.BitsPerPixel);
    }

    CHECK (EFI_ERROR (S));
    return;
  }

  if (EFI_ERROR (S) || (I.Base != Base) || (I.Width != W) || (I.Height != H) || (I.Stride != Stride) ||
      (I.BitsPerPixel != Bpp) || (I.VopBase != Vop))
  {
    printf ("FAIL: framebuffer '%s': status 0x%lx 0x%llx %ux%u stride %u %u bpp vop 0x%lx\n", What, (unsigned long)S,
            (unsigned long long)I.Base, I.Width, I.Height, I.Stride, I.BitsPerPixel, (unsigned long)I.VopBase);
  }

  CHECK (!EFI_ERROR (S));
  CHECK_EQ (I.Base, Base);
  CHECK_EQ (I.Width, W);
  CHECK_EQ (I.Height, H);
  CHECK_EQ (I.Stride, Stride);
  CHECK_EQ (I.BitsPerPixel, Bpp);
  CHECK_EQ (I.VopBase, Vop);
  // Whatever is reported can be drawn on: a line holds the visible pixels.
  CHECK (I.Stride >= I.Width * (I.BitsPerPixel / 8));
}

static void TestVop (void)
{
  int  Bpp;

  // After reset nothing is on.
  VopReset ();
  ExpectFb ("reset state", FALSE, 0, 0, 0, 0, 0, 0);

  // U-Boot's logo (lcd_init: 24 bpp RGB888, xvir = ALIGN(w * 24, 32) / 32 words).
  for (Bpp = 16; Bpp <= 32; Bpp += 8) {
    UINT32  Stride = ((640 * Bpp + 31) / 32) * 4;
    char    Name[32];

    VopReset ();
    ub_vop_show (&vopb, &Mode640, Bpp, 640, 480, 0x1DC00000);
    snprintf (Name, sizeof (Name), "VOPB 640x480 %d bpp", Bpp);
    ExpectFb (Name, TRUE, 0x1DC00000, 640, 480, Stride, Bpp, RK3326_VOPB_BASE);
  }

  // Odd widths: the stride is rounded up to whole words.
  VopReset ();
  ub_vop_show (&vopb, &Mode480, 24, 479, 320, 0x3DC00000);
  ExpectFb ("479 px 24 bpp", TRUE, 0x3DC00000, 479, 320, ((479 * 24 + 31) / 32) * 4, 24, RK3326_VOPB_BASE);
  VopReset ();
  ub_vop_show (&vopb, &Mode480, 16, 481, 320, 0x3DC00000);
  ExpectFb ("481 px 16 bpp", TRUE, 0x3DC00000, 481, 320, ((481 * 16 + 31) / 32) * 4, 16, RK3326_VOPB_BASE);

  // The little VOP when the big one is off; the big one wins when both run.
  VopReset ();
  ub_vop_show (&vopl, &Mode480, 32, 480, 320, 0x1E000000);
  ExpectFb ("VOPL only", TRUE, 0x1E000000, 480, 320, 480 * 4, 32, RK3326_VOPL_BASE);
  ub_vop_show (&vopb, &Mode640, 24, 640, 480, 0x1DC00000);
  ExpectFb ("both", TRUE, 0x1DC00000, 640, 480, 1920, 24, RK3326_VOPB_BASE);

  // Disabled window, standby, no address, no stride.
  VopReset ();
  ub_vop_show (&vopb, &Mode640, 24, 640, 480, 0x1DC00000);
  vopb.win1_ctrl0 &= ~1U;
  ExpectFb ("window disabled", FALSE, 0, 0, 0, 0, 0, 0);
  vopb.win1_ctrl0 |= 1U;
  vopb.sys_ctrl2 |= 1U << 1;
  ExpectFb ("VOP in standby", FALSE, 0, 0, 0, 0, 0, 0);
  vopb.sys_ctrl2 &= ~(1U << 1);
  vopb.win1_mst = 0;
  ExpectFb ("no address", FALSE, 0, 0, 0, 0, 0, 0);
  vopb.win1_mst = 0x1DC00000;
  vopb.win1_vir = 0;
  ExpectFb ("no stride", FALSE, 0, 0, 0, 0, 0, 0);
  vopb.win1_vir = 0xE000 | 480;       // bits above the 13-bit field are not stride
  ExpectFb ("stride with junk above bit 12", TRUE, 0x1DC00000, 640, 480, 1920, 24, RK3326_VOPB_BASE);

  // Formats other than ARGB8888/RGB888/RGB565 (YUV and reserved codes).
  {
    UINT32  Fmt;

    for (Fmt = 3; Fmt <= 7; Fmt++) {
      VopReset ();
      ub_vop_show (&vopb, &Mode640, 32, 640, 480, 0x1DC00000);
      vopb.win1_ctrl0 = (vopb.win1_ctrl0 & ~(7U << 4)) | (Fmt << 4);
      ExpectFb ("YUV/reserved format", FALSE, 0, 0, 0, 0, 0, 0);
    }
  }

  // A stride too short for the width.
  VopReset ();
  ub_vop_show (&vopb, &Mode640, 32, 640, 480, 0x1DC00000);
  vopb.win1_vir = 639;
  ExpectFb ("stride one pixel short", FALSE, 0, 0, 0, 0, 0, 0);
  vopb.win1_vir = 0x1FFF;
  ExpectFb ("widest stride", TRUE, 0x1DC00000, 640, 480, 0x1FFF * 4, 32, RK3326_VOPB_BASE);

  // A window size register left at 0: the display timing gives the size.
  VopReset ();
  ub_vop_show (&vopb, &Mode640, 32, 640, 480, 0x1DC00000);
  vopb.win1_dsp_info = 0;
  ExpectFb ("size from the timing", TRUE, 0x1DC00000, 640, 480, 2560, 32, RK3326_VOPB_BASE);
  // ... and if the timing is nonsense too (end before start), nothing.
  vopb.hact_st_end = (500U << 16) | 100;
  ExpectFb ("timing end before start", FALSE, 0, 0, 0, 0, 0, 0);

  // Larger than any panel the VOP drives (4096): refused.
  VopReset ();
  ub_vop_show (&vopb, &Mode640, 16, 640, 480, 0x1DC00000);
  vopb.win1_dsp_info = (4096U << 16) | 639;
  ExpectFb ("4097 lines", FALSE, 0, 0, 0, 0, 0, 0);

  // Random VOP contents: never a stride shorter than a line.
  {
    int  i;

    for (i = 0; i < 200000; i++) {
      RK_FRAMEBUFFER_INFO  I;

      vopb.sys_ctrl2 = Rand () & 0xFFFFFFFD;
      vopb.win1_ctrl0 = Rand () | 1;
      vopb.win1_vir = Rand () & 0x1FFF;
      vopb.win1_mst = Rand () << 12;
      vopb.win1_dsp_info = (Rand () << 16) | (Rand () & 0xFFFF);
      vopb.hact_st_end = Rand () ^ (Rand () << 16);
      vopb.vact_st_end = Rand () ^ (Rand () << 16);
      vopl.sys_ctrl2 = 2;
      if (!EFI_ERROR (RkGetActiveFramebuffer (&I))) {
        CHECK (I.Stride >= I.Width * (I.BitsPerPixel / 8));
        CHECK ((I.Width >= 16) && (I.Width <= 4096) && (I.Height >= 16) && (I.Height <= 4096));
        CHECK ((I.BitsPerPixel == 16) || (I.BitsPerPixel == 24) || (I.BitsPerPixel == 32));
      }
    }
  }

  // PD_VO off (PMU_PWRDN_ST bit 13) or its bus idle (BUS_IDLE_ST bits 7/23):
  // the VOP registers must not be read at all (the read hangs the bus).
  VopReset ();
  ub_vop_show (&vopb, &Mode640, 32, 640, 480, 0x1DC00000);
  ExpectFb ("PD_VO on", TRUE, 0x1DC00000, 640, 480, 2560, 32, RK3326_VOPB_BASE);
  pmu_pwrdn_st = 1U << 13;
  ExpectFb ("PD_VO off", FALSE, 0, 0, 0, 0, 0, 0);
  pmu_pwrdn_st = ~(1U << 13);
  ExpectFb ("other domains off", TRUE, 0x1DC00000, 640, 480, 2560, 32, RK3326_VOPB_BASE);
  pmu_pwrdn_st = 0;
  pmu_bus_idle_st = 1U << 7;
  ExpectFb ("VO bus idle", FALSE, 0, 0, 0, 0, 0, 0);
  pmu_bus_idle_st = 1U << 23;
  ExpectFb ("VO bus idle ack", FALSE, 0, 0, 0, 0, 0, 0);
  pmu_bus_idle_st = 0;
}

int main (void)
{
  void  *Lo, *Tags, *Hi;

  TestDramNamed ();
  TestDramEncoder ();
  TestDramRandom ();
  TestVop ();

  // The tags live at 0x1FE000; no-access pages on both sides.
  Lo   = mmap ((void *)(UB_ATAGS_PHYS_BASE - 0x1000), 0x1000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  Tags = mmap ((void *)UB_ATAGS_PHYS_BASE, UB_ATAGS_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  Hi   = mmap ((void *)0x200000, 0x1000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if ((Lo != (void *)(UB_ATAGS_PHYS_BASE - 0x1000)) || (Tags != (void *)UB_ATAGS_PHYS_BASE) || (Hi != (void *)0x200000)) {
    printf ("FAIL: cannot map the tag area at 0x%x on this host\n", UB_ATAGS_PHYS_BASE);
    return 1;
  }

  {
    void  *Tos = mmap ((void *)UB_PARAM_OPTEE_INFO_OFFSET, 0x1000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (Tos != (void *)UB_PARAM_OPTEE_INFO_OFFSET) {
      printf ("FAIL: cannot map the OP-TEE block at 0x%x on this host\n", UB_PARAM_OPTEE_INFO_OFFSET);
      return 1;
    }
  }

  TestTags ();
  TestLegacyTos ();
  DONE_KNOWN ();
}

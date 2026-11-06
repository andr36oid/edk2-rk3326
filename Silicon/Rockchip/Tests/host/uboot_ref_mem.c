// U-Boot's code for the miniloader tags, the DDR record and the logo's VOP
// window, copied from the andr36oid U-Boot tree (/andr36oid/work/u-boot,
// HEAD d16f5c6327) with only the I/O and printing reduced. Every function
// names its origin. Nothing here is shared with the firmware, so the tests
// compare two independent copies.
//
// SPDX-License-Identifier: GPL-2.0+

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include "uboot_ref_mem.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef unsigned long ulong;

// js_hash hashes plain chars; ARM's ABI makes char unsigned and the Makefile
// builds with -funsigned-char so this copy hashes like the miniloader.
_Static_assert ((char)-1 > 0, "build with -funsigned-char (char is unsigned on ARM)");

#define printf(...)  do { } while (0)
#define printascii(s)  do { } while (0)
#define debug(...)  do { } while (0)
#define CONFIG_IS_ENABLED(x)  0

// ---- arch/arm/include/asm/arch-rockchip/rk_atags.h ---------------------------

#define ATAG_CORE		0x54410001
#define ATAG_NONE		0x00000000
#define ATAG_SERIAL		0x54410050
#define ATAG_BOOTDEV		0x54410051
#define ATAG_DDR_MEM		0x54410052
#define ATAG_TOS_MEM		0x54410053
#define ATAG_RAM_PARTITION	0x54410054
#define ATAG_ATF_MEM		0x54410055
#define ATAG_PUB_KEY		0x54410056
#define ATAG_SOC_INFO		0x54410057
#define ATAG_MAX		0x544100ff
#define ATAGS_SIZE		(0x2000)	/* 8K */
#define ATAGS_OFFSET		(0x200000 - ATAGS_SIZE)/* [2M-8K, 2M] */
#define CONFIG_SYS_SDRAM_BASE	0
#define ATAGS_PHYS_BASE		(CONFIG_SYS_SDRAM_BASE + ATAGS_OFFSET)
#define __packed __attribute__ ((packed))
#define __aligned(x) __attribute__ ((aligned (x)))

struct tag_serial {
	u32 version;
	u32 enable;
	u64 addr;
	u32 baudrate;
	u32 m_mode;
	u32 id;
	u32 reserved[2];
	u32 hash;
} __packed;

struct tag_bootdev {
	u32 version;
	u32 devtype;
	u32 devnum;
	u32 mode;
	u32 sdupdate;
	u32 reserved[6];
	u32 hash;
} __packed;

struct tag_ddr_mem {
	u32 count;
	u32 version;
	u64 bank[20];
	u32 reserved[3];
	u32 hash;
} __packed;

struct tag_tos_mem {
	u32 version;
	struct {
		char name[8];
		u64 phy_addr;
		u32 size;
		u32 flags;
	} tee_mem;

	struct {
		char name[8];
		u64 phy_addr;
		u32 size;
		u32 flags;
	} drm_mem;

	u64 reserved[7];
	u32 reserved1;
	u32 hash;
} __packed;

struct tag_atf_mem {
	u32 version;
	u64 phy_addr;
	u32 size;
	u32 flags;
	u32 reserved[2];
	u32 hash;
} __packed;

struct tag_pub_key {
	u32 version;
	u32 len;
	u8  data[768];
	u32 reserved[6];
	u32 hash;
} __packed;

struct tag_ram_partition {
	u32 version;
	u32 count;
	u32 reserved[4];

	struct {
		char name[16];
		u64 start;
		u64 size;
	} part[6];

	u32 reserved1[3];
	u32 hash;
} __packed;

struct tag_soc_info {
	u32 version;
	u32 name;	/* Hex: 0x3288, 0x3399... */
	u32 flags;
	u32 reserved[6];
	u32 hash;
} __packed;

struct tag_core {
	u32 flags;
	u32 pagesize;
	u32 rootdev;
} __packed;

struct tag_header {
	u32 size;	/* bytes = size * 4 */
	u32 magic;
} __packed;

/* Must be 4 bytes align */
struct tag {
	struct tag_header hdr;
	union {
		struct tag_core		core;
		struct tag_serial	serial;
		struct tag_bootdev	bootdev;
		struct tag_ddr_mem	ddr_mem;
		struct tag_tos_mem	tos_mem;
		struct tag_ram_partition ram_part;
		struct tag_atf_mem	atf_mem;
		struct tag_pub_key	pub_key;
		struct tag_soc_info	soc;
	} u;
} __aligned(4);

_Static_assert (sizeof (struct tag_bootdev) == sizeof (struct ub_tag_bootdev), "layout");
_Static_assert (sizeof (struct tag_tos_mem) == sizeof (struct ub_tag_tos_mem), "layout");
_Static_assert (sizeof (struct tag_serial) == sizeof (struct ub_tag_serial), "layout");
_Static_assert (sizeof (struct tag_ddr_mem) == sizeof (struct ub_tag_ddr_mem), "layout");
_Static_assert (sizeof (struct tag_atf_mem) == sizeof (struct ub_tag_atf_mem), "layout");
_Static_assert (sizeof (struct tag_soc_info) == sizeof (struct ub_tag_soc_info), "layout");

// ---- arch/arm/mach-rockchip/rk_atags.c ---------------------------------------

#define HASH_LEN	sizeof(u32)
#define tag_next(t)	((struct tag *)((u32 *)(t) + (t)->hdr.size))
#define tag_size(type)	((sizeof(struct tag_header) + sizeof(struct type)) >> 2)
#define for_each_tag(t, base)		\
	for (t = base; t->hdr.size; t = tag_next(t))

static u32 js_hash(void *buf, u32 len)
{
	u32 i, hash = 0x47C6A7E6;
	char *data = buf;

	if (!buf || !len)
		return hash;

	for (i = 0; i < len; i++)
		hash ^= ((hash << 5) + data[i] + (hash >> 2));

	return hash;
}

static int bad_magic(u32 magic)
{
	bool bad;

	bad = ((magic != ATAG_CORE) &&
	       (magic != ATAG_NONE) &&
	       (magic < ATAG_SERIAL || magic > ATAG_MAX));
	if (bad) {
		printf("Magic(%x) is not support\n", magic);
	}

	return bad;
}

static int inline atags_size_overflow(struct tag *t, u32 tag_size)
{
	return (unsigned long)t + (tag_size << 2) - ATAGS_PHYS_BASE > ATAGS_SIZE;
}

static int atags_overflow(struct tag *t)
{
	bool overflow;

	overflow = atags_size_overflow(t, 0) ||
		   atags_size_overflow(t, t->hdr.size);
	if (overflow) {
		printf("Tag is overflow\n");
	}

	return overflow;
}

static int atags_is_available(void)
{
	struct tag *t = (struct tag *)ATAGS_PHYS_BASE;

	return (t->hdr.magic == ATAG_CORE);
}

// CONFIG_TPL_BUILD flavour: the first pre-loader creates the CORE tag.
static int atags_set_tag(u32 magic, void *tagdata)
{
	u32 length, size = 0, hash;
	struct tag *t = (struct tag *)ATAGS_PHYS_BASE;

	if (!tagdata)
		return -ENODATA;

	if (bad_magic(magic))
		return -EINVAL;

	/* Not allowed to be set by user directly, so do nothing */
	if ((magic == ATAG_CORE) || (magic == ATAG_NONE))
		return -EPERM;

	/* If not initialized, setup now! */
	if (t->hdr.magic != ATAG_CORE) {
		t->hdr.magic = ATAG_CORE;
		t->hdr.size = tag_size(tag_core);
		t->u.core.flags = 0;
		t->u.core.pagesize = 0;
		t->u.core.rootdev = 0;

		t = tag_next(t);
	} else {
		/* Find the end, and use it as a new tag */
		for_each_tag(t, (struct tag *)ATAGS_PHYS_BASE) {
			if (atags_overflow(t))
				return -EINVAL;

			if (bad_magic(t->hdr.magic))
				return -EINVAL;

			/* This is an old tag, override it */
			if (t->hdr.magic == magic)
				break;

			if (t->hdr.magic == ATAG_NONE)
				break;
		}
	}

	/* Initialize new tag */
	switch (magic) {
	case ATAG_SERIAL:
		size = tag_size(tag_serial);
		break;
	case ATAG_BOOTDEV:
		size = tag_size(tag_bootdev);
		break;
	case ATAG_TOS_MEM:
		size = tag_size(tag_tos_mem);
		break;
	case ATAG_DDR_MEM:
		size = tag_size(tag_ddr_mem);
		break;
	case ATAG_RAM_PARTITION:
		size = tag_size(tag_ram_partition);
		break;
	case ATAG_ATF_MEM:
		size = tag_size(tag_atf_mem);
		break;
	case ATAG_PUB_KEY:
		size = tag_size(tag_pub_key);
		break;
	case ATAG_SOC_INFO:
		size = tag_size(tag_soc_info);
		break;
	};

	if (!size)
		return -EINVAL;

	if (atags_size_overflow(t, size))
		return -ENOMEM;

	/* It's okay to setup a new tag */
	t->hdr.magic = magic;
	t->hdr.size = size;
	length = (t->hdr.size << 2) - sizeof(struct tag_header) - HASH_LEN;
	memcpy(&t->u, (char *)tagdata, length);
	hash = js_hash(t, (size << 2) - HASH_LEN);
	memcpy((char *)&t->u + length, &hash, HASH_LEN);

	/* Next tag */
	t = tag_next(t);

	/* Setup done */
	t->hdr.magic = ATAG_NONE;
	t->hdr.size = 0;

	return 0;
}

static struct tag *atags_get_tag(u32 magic)
{
	u32 *hash, calc_hash, size;
	struct tag *t;

	if (!atags_is_available())
		return NULL;

	for_each_tag(t, (struct tag *)ATAGS_PHYS_BASE) {
		if (atags_overflow(t))
			return NULL;

		if (bad_magic(t->hdr.magic))
			return NULL;

		if (t->hdr.magic != magic)
			continue;

		size = t->hdr.size;
		hash = (u32 *)((ulong)t + (size << 2) - HASH_LEN);
		if (!*hash) {
			debug("No hash, magic(%x)\n", magic);
			return t;
		} else {
			calc_hash = js_hash(t, (size << 2) - HASH_LEN);
			if (calc_hash == *hash) {
				debug("Hash okay, magic(%x)\n", magic);
				return t;
			} else {
				debug("Hash bad, magic(%x), orgHash=%x, nowHash=%x\n",
				      magic, *hash, calc_hash);
				return NULL;
			}
		}
	}

	return NULL;
}

static void atags_destroy(void)
{
	if (atags_is_available())
		memset((char *)ATAGS_PHYS_BASE, 0, sizeof(struct tag));
}

int ub_atags_set_tag (uint32_t magic, void *tagdata) { return atags_set_tag (magic, tagdata); }
void *ub_atags_get_tag (uint32_t magic) { return atags_get_tag (magic); }
void ub_atags_destroy (void) { atags_destroy (); }
uint32_t ub_js_hash (void *buf, uint32_t len) { return js_hash (buf, len); }

uint32_t ub_tag_words (uint32_t magic)
{
	switch (magic) {
	case ATAG_SERIAL:	return tag_size(tag_serial);
	case ATAG_BOOTDEV:	return tag_size(tag_bootdev);
	case ATAG_TOS_MEM:	return tag_size(tag_tos_mem);
	case ATAG_DDR_MEM:	return tag_size(tag_ddr_mem);
	case ATAG_ATF_MEM:	return tag_size(tag_atf_mem);
	case ATAG_SOC_INFO:	return tag_size(tag_soc_info);
	case ATAG_CORE:		return tag_size(tag_core);
	}
	return 0;
}

// ---- arch/arm/mach-rockchip/param.c (ATAG halves; the legacy OP-TEE info
// block at 34 MB is not modelled: no tag means no OP-TEE region here) ----------

// ---- arch/arm/mach-rockchip/param.c (legacy block at DDR + 34 MB) ---------

static uint16_t trust_checksum(const uint8_t *buf, uint16_t len)
{
	uint16_t i, checksum = 0;

	for (i = 0; i < len; i++) {
		if (i % 2)
			checksum += buf[i] << 8;
		else
			checksum += buf[i];
	}
	checksum = ~checksum;

	return checksum;
}

void ub_param_parse_optee_mem (uint64_t *base, uint64_t *size)
{
	struct ub_tos_param_t *tos_parameter;
	struct tag *t = NULL;
	uint32_t checksum;

	*base = 0;
	*size = 0;
	t = atags_get_tag(ATAG_TOS_MEM);
	if (t && (t->u.tos_mem.tee_mem.flags == 1)) {
		*base = t->u.tos_mem.tee_mem.phy_addr;
		*size = t->u.tos_mem.tee_mem.size;
	}

	/* Legacy */
	if (!*size) {
		tos_parameter = (struct ub_tos_param_t *)(uintptr_t)UB_PARAM_OPTEE_INFO_OFFSET;
		checksum = trust_checksum((uint8_t *)(unsigned long)tos_parameter + 8,
					  sizeof(struct ub_tos_param_t) - 8);
		if ((checksum == tos_parameter->checksum) &&
		    (tos_parameter->tee_mem.flags == 1)) {
			*base = tos_parameter->tee_mem.phy_addr;
			*size = tos_parameter->tee_mem.size;
		}
	}
}

uint32_t ub_trust_checksum (const void *buf, uint32_t len)
{
	return trust_checksum(buf, (uint16_t)len);
}

int ub_param_bootdev_type (uint32_t *devtype)
{
	struct tag *t;

	t = atags_get_tag(ATAG_BOOTDEV);
	if (t) {
		*devtype = t->u.bootdev.devtype;
		return 0;
	}

	return -ENOSYS;
}

// ---- arch/arm/include/asm/arch-rockchip/sdram_common.h ----------------------

enum {
	DDR4 = 0,
	DDR2 = 2,
	DDR3 = 3,
	LPDDR2 = 5,
	LPDDR3 = 6,
	LPDDR4 = 7,
	UNUSED = 0xFF
};

#define SYS_REG_DDRTYPE_SHIFT		13
#define SYS_REG_DDRTYPE_MASK		7
#define SYS_REG_NUM_CH_SHIFT		12
#define SYS_REG_NUM_CH_MASK		1
#define SYS_REG_ROW_3_4_SHIFT(ch)	(30 + (ch))
#define SYS_REG_ROW_3_4_MASK		1
#define SYS_REG_CHINFO_SHIFT(ch)	(28 + (ch))
#define SYS_REG_RANK_SHIFT(ch)		(11 + (ch) * 16)
#define SYS_REG_RANK_MASK		1
#define SYS_REG_COL_SHIFT(ch)		(9 + (ch) * 16)
#define SYS_REG_COL_MASK		3
#define SYS_REG_BK_SHIFT(ch)		(8 + (ch) * 16)
#define SYS_REG_BK_MASK			1
#define SYS_REG_CS0_ROW_SHIFT(ch)	(6 + (ch) * 16)
#define SYS_REG_CS0_ROW_MASK		3
#define SYS_REG_CS1_ROW_SHIFT(ch)	(4 + (ch) * 16)
#define SYS_REG_CS1_ROW_MASK		3
#define SYS_REG_BW_SHIFT(ch)		(2 + (ch) * 16)
#define SYS_REG_BW_MASK			3
#define SYS_REG_DBW_SHIFT(ch)		((ch) * 16)
#define SYS_REG_DBW_MASK		3

#define SYS_REG1_VERSION_SHIFT			28
#define SYS_REG1_VERSION_MASK			0xf
#define SYS_REG1_EXTEND_CS0_ROW_SHIFT(ch)	(5 + (ch) * 2)
#define SYS_REG1_EXTEND_CS0_ROW_MASK		1
#define SYS_REG1_EXTEND_CS1_ROW_SHIFT(ch)	(4 + (ch) * 2)
#define SYS_REG1_EXTEND_CS1_ROW_MASK		1
#define SYS_REG1_CS1_COL_SHIFT(ch)		(0 + (ch) * 2)
#define SYS_REG1_CS1_COL_MASK			3

#define SDRAM_MAX_SIZE			0xff000000   /* include/configs/px30_common.h */

// ---- arch/arm/mach-rockchip/sdram_common.c: rockchip_sdram_size -------------
// readl() reads the two words the caller passes.

typedef uintptr_t phys_addr_t;
#define readl(a)  (*(volatile u32 *)(a))

static size_t rockchip_sdram_size(phys_addr_t reg)
{
	u32 rank, cs0_col, bk, cs0_row, cs1_row, bw, row_3_4;
	size_t chipsize_mb = 0;
	size_t size_mb = 0;
	u32 ch;
	u32 cs1_col = 0;
	u32 bg = 0;
	u32 dbw, dram_type;
	u32 sys_reg = readl(reg);
	u32 sys_reg1 = readl(reg + 4);
	u32 ch_num = 1 + ((sys_reg >> SYS_REG_NUM_CH_SHIFT)
		       & SYS_REG_NUM_CH_MASK);

	dram_type = (sys_reg >> SYS_REG_DDRTYPE_SHIFT) & SYS_REG_DDRTYPE_MASK;
	debug("%s %x %x\n", __func__, (u32)reg, sys_reg);
	for (ch = 0; ch < ch_num; ch++) {
		rank = 1 + (sys_reg >> SYS_REG_RANK_SHIFT(ch) &
			SYS_REG_RANK_MASK);
		cs0_col = 9 + (sys_reg >> SYS_REG_COL_SHIFT(ch) & SYS_REG_COL_MASK);
		cs1_col = cs0_col;
		bk = 3 - ((sys_reg >> SYS_REG_BK_SHIFT(ch)) & SYS_REG_BK_MASK);
		if ((sys_reg1 >> SYS_REG1_VERSION_SHIFT &
		     SYS_REG1_VERSION_MASK) == 0x2) {
			cs1_col = 9 + (sys_reg1 >> SYS_REG1_CS1_COL_SHIFT(ch) &
				  SYS_REG1_CS1_COL_MASK);
			if (((sys_reg1 >> SYS_REG1_EXTEND_CS0_ROW_SHIFT(ch) &
			    SYS_REG1_EXTEND_CS0_ROW_MASK) << 2) + (sys_reg >>
			    SYS_REG_CS0_ROW_SHIFT(ch) &
			    SYS_REG_CS0_ROW_MASK) == 7)
				cs0_row = 12;
			else
				cs0_row = 13 + (sys_reg >>
					  SYS_REG_CS0_ROW_SHIFT(ch) &
					  SYS_REG_CS0_ROW_MASK) +
					  ((sys_reg1 >>
					  SYS_REG1_EXTEND_CS0_ROW_SHIFT(ch) &
					  SYS_REG1_EXTEND_CS0_ROW_MASK) << 2);
			if (((sys_reg1 >> SYS_REG1_EXTEND_CS1_ROW_SHIFT(ch) &
			    SYS_REG1_EXTEND_CS1_ROW_MASK) << 2) + (sys_reg >>
			    SYS_REG_CS1_ROW_SHIFT(ch) &
			    SYS_REG_CS1_ROW_MASK) == 7)
				cs1_row = 12;
			else
				cs1_row = 13 + (sys_reg >>
					  SYS_REG_CS1_ROW_SHIFT(ch) &
					  SYS_REG_CS1_ROW_MASK) +
					  ((sys_reg1 >>
					  SYS_REG1_EXTEND_CS1_ROW_SHIFT(ch) &
					  SYS_REG1_EXTEND_CS1_ROW_MASK) << 2);
		}
		else {
			cs0_row = 13 + (sys_reg >> SYS_REG_CS0_ROW_SHIFT(ch) &
				SYS_REG_CS0_ROW_MASK);
			cs1_row = 13 + (sys_reg >> SYS_REG_CS1_ROW_SHIFT(ch) &
				SYS_REG_CS1_ROW_MASK);
		}
		bw = (2 >> ((sys_reg >> SYS_REG_BW_SHIFT(ch)) &
			SYS_REG_BW_MASK));
		row_3_4 = sys_reg >> SYS_REG_ROW_3_4_SHIFT(ch) &
			SYS_REG_ROW_3_4_MASK;
		if (dram_type == DDR4) {
			dbw = (sys_reg >> SYS_REG_DBW_SHIFT(ch)) &
				SYS_REG_DBW_MASK;
			bg = (dbw == 2) ? 2 : 1;
		}
		chipsize_mb = (1 << (cs0_row + cs0_col + bk + bg + bw - 20));

		if (rank > 1)
			chipsize_mb += chipsize_mb >> ((cs0_row - cs1_row) +
				       (cs0_col - cs1_col));
		if (row_3_4)
			chipsize_mb = chipsize_mb * 3 / 4;
		size_mb += chipsize_mb;
	}

	/* Handle 4GB size, or else size will be 0 after <<20 in 32bit system */
	if (size_mb > (SDRAM_MAX_SIZE >> 20))
		size_mb = (SDRAM_MAX_SIZE >> 20);

	return (size_t)size_mb << 20;
}

size_t ub_rockchip_sdram_size (uint32_t os_reg2, uint32_t os_reg3)
{
	u32 regs[2] = { os_reg2, os_reg3 };

	return rockchip_sdram_size ((phys_addr_t)regs);
}

// ---- arch/arm/include/asm/arch-rockchip/sdram_share.h ------------------------

#define DDR_SYS_REG_VERSION		(0x2)
#define SYS_REG_ENC_ROW_3_4(n, ch)	((n) << (30 + (ch)))
#define SYS_REG_ENC_CHINFO(ch)		(1 << (28 + (ch)))
#define SYS_REG_ENC_DDRTYPE(n)		((n) << 13)
#define SYS_REG_ENC_NUM_CH(n)		(((n) - 1) << 12)
#define SYS_REG_ENC_RANK(n, ch)		(((n) - 1) << (11 + ((ch) * 16)))
#define SYS_REG_ENC_COL(n, ch)		(((n) - 9) << (9 + ((ch) * 16)))
#define SYS_REG_ENC_BK(n, ch)		(((n) == 3 ? 0 : 1) << \
						(8 + ((ch) * 16)))
#define SYS_REG_ENC_BW(n, ch)		((2 >> (n)) << (2 + ((ch) * 16)))
#define SYS_REG_ENC_DBW(n, ch)		((2 >> (n)) << (0 + ((ch) * 16)))
/* sys reg 3 */
#define SYS_REG_ENC_VERSION(n)		((n) << 28)
#define SYS_REG_ENC_CS0_ROW(n, os_reg2, os_reg3, ch) do { \
			(os_reg2) |= (((n) - 13) & 0x3) << (6 + 16 * (ch)); \
			(os_reg3) |= ((((n) - 13) & 0x4) >> 2) << \
				     (5 + 2 * (ch)); \
		} while (0)

#define SYS_REG_ENC_CS1_ROW(n, os_reg2, os_reg3, ch) do { \
			(os_reg2) &= (~(0x3 << (4 + 16 * (ch)))); \
			(os_reg3) &= (~(0x1 << (4 + 2 * (ch)))); \
			(os_reg2) |= (((n) - 13) & 0x3) << (4 + 16 * (ch)); \
			(os_reg3) |= ((((n) - 13) & 0x4) >> 2) << \
				     (4 + 2 * (ch)); \
		} while (0)

#define SYS_REG_ENC_CS1_COL(n, ch)	(((n) - 9) << (0 + 2 * (ch)))

// ---- drivers/ram/rockchip/sdram_share.c --------------------------------------

#define sdram_cap_info ub_sdram_cap_info
#define sdram_base_params ub_sdram_base_params

void ub_sdram_org_config(struct sdram_cap_info *cap_info,
		      struct sdram_base_params *base,
		      u32 *p_os_reg2, u32 *p_os_reg3, u32 channel)
{
	*p_os_reg2 |= SYS_REG_ENC_DDRTYPE(base->dramtype);
	*p_os_reg2 |= SYS_REG_ENC_NUM_CH(base->num_channels);

	*p_os_reg2 |= SYS_REG_ENC_ROW_3_4(cap_info->row_3_4, channel);
	*p_os_reg2 |= SYS_REG_ENC_CHINFO(channel);
	*p_os_reg2 |= SYS_REG_ENC_RANK(cap_info->rank, channel);
	*p_os_reg2 |= SYS_REG_ENC_COL(cap_info->col, channel);
	*p_os_reg2 |= SYS_REG_ENC_BK(cap_info->bk, channel);
	*p_os_reg2 |= SYS_REG_ENC_BW(cap_info->bw, channel);
	*p_os_reg2 |= SYS_REG_ENC_DBW(cap_info->dbw, channel);

	SYS_REG_ENC_CS0_ROW(cap_info->cs0_row, *p_os_reg2, *p_os_reg3, channel);
	if (cap_info->cs1_row)
		SYS_REG_ENC_CS1_ROW(cap_info->cs1_row, *p_os_reg2,
				    *p_os_reg3, channel);
	*p_os_reg3 |= SYS_REG_ENC_CS1_COL(cap_info->col, channel);
	*p_os_reg3 |= SYS_REG_ENC_VERSION(DDR_SYS_REG_VERSION);
}

/*
 * cs: 0:cs0
 *	   1:cs1
 *     else cs0+cs1
 * note: it didn't consider about row_3_4
 */
u64 ub_sdram_get_cs_cap(struct sdram_cap_info *cap_info, u32 cs, u32 dram_type)
{
	u32 bg;
	u64 cap[2];

	if (dram_type == DDR4)
		/* DDR4 8bit dram BG = 2(4bank groups),
		 * 16bit dram BG = 1 (2 bank groups)
		 */
		bg = (cap_info->dbw == 0) ? 2 : 1;
	else
		bg = 0;
	cap[0] = 1llu << (cap_info->bw + cap_info->col +
		bg + cap_info->bk + cap_info->cs0_row);

	if (cap_info->rank == 2)
		cap[1] = 1llu << (cap_info->bw + cap_info->col +
			bg + cap_info->bk + cap_info->cs1_row);
	else
		cap[1] = 0;

	if (cs == 0)
		return cap[0];
	else if (cs == 1)
		return cap[1];
	else
		return (cap[0] + cap[1]);
}

// ---- drivers/video/drm: the logo's window ------------------------------------
// rockchip_vop_reg.c (px30_ctrl_data, rk3366_win1_data):
//   standby      SYS_CTRL2 0x18 bit 1      hact_st_end 0x104 mask 0x0fff0fff
//   win1 enable  CTRL0 0x90 bit 0          vact_st_end 0x10c mask 0x0fff0fff
//   win1 format  CTRL0 bits 6:4            win1 rb_swap CTRL0 bit 12
//   yrgb_vir     0x98 mask 0x1fff          yrgb_mst 0xa0, dsp_info 0xa4

#define ALIGN(x, a)  (((x) + (a) - 1) & ~((a) - 1))
#define VOP_SET(reg, mask, shift, v)  ((reg) = ((reg) & ~((u32)(mask) << (shift))) | (((u32)(v) & (mask)) << (shift)))

void ub_vop_show (UB_VOP_REGS *r, const UB_MODE *mode, int bpp, int src_w, int src_h, uint32_t dma_addr)
{
	u32 val, format, xvir, dsp_info;
	// rockchip_vop_init (rockchip_vop.c:204-213, 416-424)
	u16 hdisplay = mode->hdisplay;
	u16 hact_st = mode->htotal - mode->hsync_start;
	u16 hact_end = hact_st + hdisplay;
	u16 vdisplay = mode->vdisplay;
	u16 vact_st = mode->vtotal - mode->vsync_start;
	u16 vact_end = vact_st + vdisplay;
	int crtc_w = src_w, crtc_h = src_h;     // lcd_init: crtc_w/h = src_w/h = panel size

	val = hact_st << 16;
	val |= hact_end;
	VOP_SET (r->hact_st_end, 0x0fff0fff, 0, val);
	val = vact_st << 16;
	val |= vact_end;
	VOP_SET (r->vact_st_end, 0x0fff0fff, 0, val);

	// display_logo / lcd_init: format by bpp, xvir in 32-bit words
	switch (bpp) {
	case 16: format = UB_FMT_RGB565; break;
	case 24: format = UB_FMT_RGB888; break;
	default: format = UB_FMT_ARGB8888; break;
	}
	xvir = ALIGN(src_w * bpp, 32) >> 5;

	// rockchip_vop_set_plane (rockchip_vop.c:663-712)
	dsp_info = (crtc_h - 1) << 16;
	dsp_info |= (crtc_w - 1) & 0xffff;
	VOP_SET (r->win1_ctrl0, 0x7, 4, format);
	VOP_SET (r->win1_vir, 0x1fff, 0, xvir);
	VOP_SET (r->win1_mst, 0xffffffff, 0, dma_addr);
	VOP_SET (r->win1_dsp_info, 0xffffffff, 0, dsp_info);
	VOP_SET (r->win1_ctrl0, 0x1, 0, 1);
	// rockchip_vop_enable
	VOP_SET (r->sys_ctrl2, 0x1, 1, 0);
}

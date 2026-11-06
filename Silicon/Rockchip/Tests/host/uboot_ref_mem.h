// U-Boot's own code for what the boot chain leaves in memory and registers
// (see uboot_ref_mem.c): the miniloader's parameter tags, the DDR record in
// PMUGRF OS_REG2/3, the VOP window U-Boot programs for its logo.
//
// SPDX-License-Identifier: GPL-2.0+

#ifndef UBOOT_REF_MEM_H_
#define UBOOT_REF_MEM_H_

#include <stdint.h>
#include <stddef.h>

// ---- rk_atags (arch/arm/include/asm/arch-rockchip/rk_atags.h) ---------------

#define UB_ATAG_CORE          0x54410001
#define UB_ATAG_NONE          0x00000000
#define UB_ATAG_SERIAL        0x54410050
#define UB_ATAG_BOOTDEV       0x54410051
#define UB_ATAG_DDR_MEM       0x54410052
#define UB_ATAG_TOS_MEM       0x54410053
#define UB_ATAG_RAM_PARTITION 0x54410054
#define UB_ATAG_ATF_MEM       0x54410055
#define UB_ATAG_PUB_KEY       0x54410056
#define UB_ATAG_SOC_INFO      0x54410057
#define UB_ATAG_MAX           0x544100ff
#define UB_ATAGS_SIZE         0x2000
#define UB_ATAGS_PHYS_BASE    (0x200000 - UB_ATAGS_SIZE)   // CONFIG_SYS_SDRAM_BASE 0 on PX30

#define UB_BOOT_TYPE_EMMC     (1 << 1)
#define UB_BOOT_TYPE_SD0      (1 << 2)
#define UB_BOOT_TYPE_SD1      (1 << 3)
#define UB_BOOT_TYPE_SPI_NOR  (1 << 4)

struct ub_tag_serial {
  uint32_t version, enable;
  uint64_t addr;
  uint32_t baudrate, m_mode, id, reserved[2], hash;
} __attribute__ ((packed));

struct ub_tag_bootdev {
  uint32_t version, devtype, devnum, mode, sdupdate, reserved[6], hash;
} __attribute__ ((packed));

struct ub_tag_ddr_mem {
  uint32_t count, version;
  uint64_t bank[20];
  uint32_t reserved[3], hash;
} __attribute__ ((packed));

struct ub_tos_region {
  char     name[8];
  uint64_t phy_addr;
  uint32_t size, flags;
} __attribute__ ((packed));

struct ub_tag_tos_mem {
  uint32_t              version;
  struct ub_tos_region  tee_mem;
  struct ub_tos_region  drm_mem;
  uint64_t              reserved[7];
  uint32_t              reserved1, hash;
} __attribute__ ((packed));

struct ub_tag_atf_mem {
  uint32_t version;
  uint64_t phy_addr;
  uint32_t size, flags, reserved[2], hash;
} __attribute__ ((packed));

struct ub_tag_soc_info {
  uint32_t version, name, flags, reserved[6], hash;
} __attribute__ ((packed));

struct ub_tag_header {
  uint32_t size;   // in 32-bit words
  uint32_t magic;
} __attribute__ ((packed));

// atags_set_tag as the first pre-loader runs it (it sets up the CORE tag).
int       ub_atags_set_tag (uint32_t magic, void *tagdata);
// atags_get_tag: the tag header, or NULL.
void     *ub_atags_get_tag (uint32_t magic);
void      ub_atags_destroy (void);
uint32_t  ub_js_hash (void *buf, uint32_t len);
uint32_t  ub_tag_words (uint32_t magic);   // tag_size() of that tag type, 0 if unknown

// What U-Boot makes of the tags (arch/arm/mach-rockchip/param.c): the OP-TEE
// region it reserves (size 0 = none) and the boot device type it acts on.
// arch/arm/mach-rockchip/param.c: the pre-ATAGs OP-TEE block at DDR + 34 MB.
#define UB_PARAM_OPTEE_INFO_OFFSET  (0x02000000 + 0x00200000)
struct ub_tos_param_t {
	uint32_t version;
	uint32_t checksum;
	struct {
		char name[8];
		int64_t phy_addr;
		uint32_t size;
		uint32_t flags;
	} tee_mem;
	struct {
		char name[8];
		int64_t phy_addr;
		uint32_t size;
		uint32_t flags;
	} drm_mem;
	int64_t reserve[8];
};
uint32_t  ub_trust_checksum (const void *buf, uint32_t len);
void      ub_param_parse_optee_mem (uint64_t *base, uint64_t *size);
int       ub_param_bootdev_type (uint32_t *devtype);

// ---- DDR record (sdram_common.c, sdram_share.c/.h) ----------------------------

enum { UB_DDR4 = 0, UB_DDR2 = 2, UB_DDR3 = 3, UB_LPDDR2 = 5, UB_LPDDR3 = 6, UB_LPDDR4 = 7 };

struct ub_sdram_cap_info {
  unsigned int rank, col, bk, bw, dbw, row_3_4, cs0_row, cs1_row, cs0_high16bit_row, cs1_high16bit_row, ddrconfig;
};

struct ub_sdram_base_params {
  unsigned int ddr_freq, dramtype, num_channels, stride, odt;
};

#define UB_SDRAM_MAX_SIZE  0xff000000ULL   // include/configs/px30_common.h

size_t    ub_rockchip_sdram_size (uint32_t os_reg2, uint32_t os_reg3);
void      ub_sdram_org_config (struct ub_sdram_cap_info *cap, struct ub_sdram_base_params *base,
                               uint32_t *os_reg2, uint32_t *os_reg3, uint32_t channel);
uint64_t  ub_sdram_get_cs_cap (struct ub_sdram_cap_info *cap, uint32_t cs, uint32_t dram_type);

// ---- VOP window for the logo (drivers/video/drm) -------------------------------

#define UB_FMT_ARGB8888  0
#define UB_FMT_RGB888    1
#define UB_FMT_RGB565    2

typedef struct {
  uint32_t sys_ctrl2, win1_ctrl0, win1_vir, win1_mst, win1_dsp_info, hact_st_end, vact_st_end;
} UB_VOP_REGS;

// A panel mode as in the screen DTB (U-Boot's drm_display_mode fields used).
typedef struct {
  uint16_t hdisplay, hsync_start, hsync_end, htotal;
  uint16_t vdisplay, vsync_start, vsync_end, vtotal;
} UB_MODE;

// rockchip_vop_init (timing) + rockchip_vop_set_plane (window) + enable, for a
// src_w x src_h image of bpp bits at dma_addr, as lcd_init sets them up.
void      ub_vop_show (UB_VOP_REGS *r, const UB_MODE *mode, int bpp, int src_w, int src_h, uint32_t dma_addr);

#endif

/*
 * andr36oid-px30: just enough of a Rockchip PX30/RK3326 handheld for the
 * andr36oid UEFI (Mu-r36s.bin) to run SEC, PEI, DXE and BDS.
 *
 * Copied into QEMU's hw/arm/ by qemu-build.sh. Not a general PX30 model: every
 * behaviour below comes from a Linux (Rockchip 4.19, andr36oid tree) or U-Boot
 * (odroidgoa fork) driver, named next to the rule. Nothing here is derived from
 * the firmware under test.
 *
 * Map (kernel arch/arm64/boot/dts/rockchip/px30.dtsi):
 *   0x00000000  RAM (machine RAM backend; 512 MB or 1 GB)
 *   0xFF131000  GIC-400 distributor, 0xFF132000 CPU interface (GICv2)
 *   0xFF160000  UART2, 16550 with reg-shift 2, 32-bit (SPI 17)
 *   0xFF300000  USB OTG: QEMU's dwc2-usb host model unless dwc2=off (SPI 62)
 *   0xFF000000-0xFFFFFFFF otherwise: a register file ("px30-regs") with the
 *               rules below; accesses no rule covers are stored like RAM and
 *               logged to the trace file with the guest PC and LR.
 * CPU: one Cortex-A35, EL2 is the highest EL (no EL3), QEMU's PSCI on SMC,
 * CNTFRQ 24 MHz. Timer PPIs as px30.dtsi: secure 13, phys 14, virt 11, hyp 10.
 *
 * Machine options (-machine andr36oid-px30,<opt>=<val>,...):
 *   entry=<hex>     PC after reset (default 0x01ffff00)
 *   x0..x3=<hex>    registers after reset (default 0)
 *   regs=<file>     register file, mmap'd (16 MB, offset = addr - 0xFF000000);
 *                   current even if QEMU is killed
 *   regs-init=<f>   text file "addr value" (hex) applied at start
 *   trace=<file>    unhandled-access log
 *   trace-all=on    log every access to the register window, not only
 *                   unhandled ones
 *   adc=<ch>=<v>[:<ch>=<v>...]       SARADC channel values (10 bit)
 *   gpio-in=<bank>=<hex>[:...]       GPIO input levels (default all high:
 *                   the buttons are active-low keys with pull-ups, so high =
 *                   nothing pressed)
 *   charger=on      RK817 SYS_STS PLUG_IN_STS set
 *   dwc2=off        no DWC2 model (the window is then plain registers)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/char/serial-mm.h"
#include "hw/intc/arm_gic.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/target_page.h"
#include <sys/mman.h>

#define REGS_BASE   0xFF000000ULL
#define REGS_SIZE   (16 * MiB)
#define NUM_IRQ     160             /* 32 internal + 128 SPIs (px30.dtsi uses up to SPI 105) */

/* ------------------------------------------------------------------------ */
/* Options                                                                   */

static struct {
    uint64_t entry;
    uint64_t x[4];
    char *regs_path;
    char *regs_init;
    char *trace_path;
    bool trace_all;
    uint32_t adc[8];
    uint32_t gpio_in[4];
    bool charger;
    bool usb;
} opt = {
    .entry = 0x01ffff00,
    .adc = { 166, 0x3ff, 0x3ff, 0x3ff, 0x3ff, 0x3ff, 0x3ff, 0x3ff },
    .gpio_in = { 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff },
    .usb = true,
};

static uint32_t *regs;
static FILE *trace_fp;
static ARMCPU *the_cpu;

static inline uint32_t *R(hwaddr abs)
{
    return &regs[(abs - REGS_BASE) >> 2];
}

/* ------------------------------------------------------------------------ */
/* Trace                                                                     */

static struct {
    char rw;
    unsigned size;
    hwaddr addr;
    uint64_t val, pc;
    uint64_t repeat;
    bool valid;
} last;
static uint64_t trace_lines, trace_seq;
#define TRACE_MAX_LINES 4000000

static void guest_pc_lr(uint64_t *pc, uint64_t *lr)
{
    CPUState *cs = current_cpu;
    uint64_t data[8] = { 0 };

    *pc = 0;
    *lr = 0;
    if (!cs) {
        return;
    }
    CPUARMState *env = &ARM_CPU(cs)->env;
    *pc = env->pc;
    /*
     * In an MMIO callback env->pc is only the start of the TB. The unwind
     * data of the host return address gives the instruction; AArch64 system
     * mode TBs are always CF_PCREL, so it is the page offset (as
     * arm_restore_state_to_opc does). env is not modified here.
     */
    if (cs->mem_io_pc && cpu_unwind_state_data(cs, cs->mem_io_pc, data)) {
        *pc = (env->pc & (uint64_t)(int64_t)qemu_target_page_mask()) | data[0];
    }
    *lr = env->xregs[30];
}

static void trace_flush_repeat(void)
{
    if (trace_fp && last.valid && last.repeat) {
        fprintf(trace_fp, "        ... same access %" PRIu64 " more times\n", last.repeat);
        trace_lines++;
    }
    last.repeat = 0;
}

static const char *block_name(hwaddr a);

static void trace_access(char rw, hwaddr a, unsigned size, uint64_t val, bool handled)
{
    uint64_t pc, lr;

    if (!trace_fp || (handled && !opt.trace_all)) {
        return;
    }
    guest_pc_lr(&pc, &lr);
    trace_seq++;
    if (last.valid && last.rw == rw && last.addr == a && last.size == size &&
        last.val == val && last.pc == pc) {
        last.repeat++;
        return;
    }
    trace_flush_repeat();
    last.valid = true;
    last.rw = rw;
    last.addr = a;
    last.size = size;
    last.val = val;
    last.pc = pc;
    if (trace_lines >= TRACE_MAX_LINES) {
        return;
    }
    fprintf(trace_fp, "%c%u %08" PRIx64 " %08" PRIx64 " pc=%010" PRIx64 " lr=%010" PRIx64 " %s%s\n",
            rw, size, (uint64_t)a, val, pc, lr, block_name(a), handled ? "" : " UNHANDLED");
    trace_lines++;
}

static void trace_note(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void trace_note(const char *fmt, ...)
{
    va_list ap;
    uint64_t pc, lr;

    if (!trace_fp) {
        return;
    }
    trace_flush_repeat();
    last.valid = false;
    guest_pc_lr(&pc, &lr);
    fprintf(trace_fp, "NOTE pc=%010" PRIx64 " lr=%010" PRIx64 " ", pc, lr);
    va_start(ap, fmt);
    vfprintf(trace_fp, fmt, ap);
    va_end(ap);
    fputc('\n', trace_fp);
    trace_lines++;
}

/* ------------------------------------------------------------------------ */
/* Rules                                                                     */

/*
 * Rockchip "hiword mask" registers (GRF, PMUGRF, CRU, PMU...): bits 31:16 of a
 * write say which of bits 15:0 change; they read back as 0. Linux writes them
 * with HIWORD_UPDATE()/regmap_write(..., (mask << 16) | val) throughout
 * drivers/pinctrl/pinctrl-rockchip.c, drivers/clk/rockchip/, pm_domains.c.
 */
static inline uint32_t hiword(uint32_t old, uint32_t v)
{
    uint32_t m = v >> 16;

    return ((old & ~m) | (v & m)) & 0xffff;
}

typedef struct Rule {
    hwaddr base, size;
    const char *name;
    /* return true when a rule decided the value; *v holds the stored word */
    bool (*rd)(hwaddr a, uint32_t *v);
    /* return true when handled (and stored) */
    bool (*wr)(hwaddr a, uint32_t old, uint32_t v, unsigned size);
} Rule;

static bool rd_plain(hwaddr a, uint32_t *v)
{
    return true;
}

static bool wr_plain(hwaddr a, uint32_t old, uint32_t v, unsigned size)
{
    *R(a) = v;
    return true;
}

static bool wr_hiword(hwaddr a, uint32_t old, uint32_t v, unsigned size)
{
    *R(a) = (size == 4) ? hiword(old, v) : v;
    return size == 4;
}

/* --- PMU (power domains), kernel drivers/soc/rockchip/pm_domains.c px30_pmu:
 * PWRDN_CON 0x18 (hiword, 1 = off), PWRDN_ST 0x20 (status masks equal the
 * power masks for every px30 domain), BUS_IDLE_REQ 0x64 (hiword), idle/ack
 * 0x6c: ack = req bit, idle = req bit << 16 (DOMAIN_PX30). The domain follows
 * the request at once. */
static bool pmu_rd(hwaddr a, uint32_t *v)
{
    switch (a & 0xfff) {
    case 0x18:
    case 0x64:
        return true;
    case 0x20:
        *v = *R(REGS_BASE + 0x18) & 0xffff;
        return true;
    case 0x6c: {
        uint32_t req = *R(REGS_BASE + 0x64) & 0xffff;
        *v = req | (req << 16);
        return true;
    }
    }
    return false;
}

static bool pmu_wr(hwaddr a, uint32_t old, uint32_t v, unsigned size)
{
    switch (a & 0xfff) {
    case 0x18:
    case 0x64:
        return wr_hiword(a, old, v, size);
    case 0x20:
    case 0x6c:
        return true;            /* status, read-only */
    }
    return false;
}

/* --- PMUGRF: GPIO0 iomux/pull/drive and the other controls are hiword
 * (pinctrl-rockchip.c px30 pmu_grf offsets, io-domain PMU_IO_VSEL); the
 * OS_REG0-3 words at 0x200-0x20c are plain (U-Boot sdram_common.c reads the
 * DDR geometry from OS_REG2/3, written by the DDR init). */
static bool pmugrf_wr(hwaddr a, uint32_t old, uint32_t v, unsigned size)
{
    uint32_t off = a & 0xfff;

    if (off >= 0x200 && off < 0x210) {
        return wr_plain(a, old, v, size);
    }
    return wr_hiword(a, old, v, size);
}

/* --- GPIO (pinctrl-rockchip.c 4.19, gpio v1): DR 0x00, DDR 0x04, INT regs
 * 0x30-0x48, EOI 0x4c, EXT_PORT 0x50 = level on the pins, LS_SYNC 0x60.
 * Outputs read back what is driven; inputs come from gpio-in. No interrupts. */
static int gpio_bank(hwaddr a)
{
    switch (a & ~0xffffULL) {
    case 0xFF040000: return 0;
    case 0xFF250000: return 1;
    case 0xFF260000: return 2;
    case 0xFF270000: return 3;
    }
    return 0;
}

static bool gpio_rd(hwaddr a, uint32_t *v)
{
    hwaddr base = a & ~0xffffULL;
    uint32_t dr = *R(base + 0x00), ddr = *R(base + 0x04);

    switch (a & 0xff) {
    case 0x00: case 0x04: case 0x30: case 0x34: case 0x38: case 0x3c:
    case 0x48: case 0x60:
        return true;
    case 0x40: case 0x44:
        *v = 0;
        return true;
    case 0x50:
        *v = (dr & ddr) | (opt.gpio_in[gpio_bank(a)] & ~ddr);
        return true;
    }
    return false;
}

static bool gpio_wr(hwaddr a, uint32_t old, uint32_t v, unsigned size)
{
    switch (a & 0xff) {
    case 0x00: case 0x04: case 0x30: case 0x34: case 0x38: case 0x3c:
    case 0x48: case 0x60:
        return wr_plain(a, old, v, size);
    case 0x4c:
        return true;
    }
    return false;
}

/* --- UART2 registers past the 16550 block (8250_dw.c): USR 0x7c reads "not
 * busy, TX FIFO not full and empty"; the rest stores. */
static bool uart_rd(hwaddr a, uint32_t *v)
{
    if ((a & 0xff) == 0x7c) {
        *v = 0x6;
        return true;
    }
    return false;
}

/* --- I2C (rk3x, kernel drivers/i2c/busses/i2c-rk3x.c): CON 0x00 (EN bit0,
 * MOD bits 2:1 0=TX 1=TRX 2=RX, START bit3, STOP bit4, LASTACK bit5),
 * CLKDIV 0x04, MRXADDR 0x08, MRXRADDR 0x0c (bit24 = low byte valid), MTXCNT
 * 0x10, MRXCNT 0x14, IEN 0x18, IPD 0x1c (W1C: MBTF bit2, MBRF bit3, START
 * bit4, STOP bit5, NAKRCV bit6), TXDATA 0x100-0x11c, RXDATA 0x200-0x21c.
 * Writing START/STOP or a count finishes that step at once. The only device
 * is the RK817 PMIC at 0x20 on I2C0 (rk3326-*.dts); others NAK. */
static uint8_t rk817[256];
static uint8_t i2c_ptr[4];

static void rk817_reset(void)
{
    memset(rk817, 0, sizeof(rk817));
    rk817[0xed] = 0x81;         /* RK817_ID_MSB/LSB: 0x8170 (rk808.h, & RK8XX_ID_MSK) */
    rk817[0xee] = 0x70;
    rk817[0xeb] = 0x80;         /* CHRG_STS BAT_EXS (rk817_charger.c REG_FIELD(0xEB, 7, 7)) */
    rk817[0xf0] = opt.charger ? 0x40 : 0x00;  /* SYS_STS PLUG_IN_STS (REG_FIELD(0xf0, 6, 6)) */
}

static void rk817_write(uint8_t reg, uint8_t val)
{
    rk817[reg] = val;
    /* rk808.c rk817 power off: RK817_SYS_CFG(3) = 0xf4 |= DEV_OFF (bit0) */
    if (reg == 0xf4 && (val & 0x01)) {
        trace_note("RK817 DEV_OFF: power off");
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
}

static int i2c_bus(hwaddr a)
{
    return (int)(((a & ~0xffffULL) - 0xFF180000ULL) >> 16);
}

static bool i2c_dev_present(int bus, uint8_t addr7)
{
    return bus == 0 && addr7 == 0x20;
}

static void i2c_ipd(hwaddr base, uint32_t bits)
{
    *R(base + 0x1c) |= bits;
}

static bool i2c_wr(hwaddr a, uint32_t old, uint32_t v, unsigned size)
{
    hwaddr base = a & ~0xffffULL;
    int bus = i2c_bus(a);
    uint32_t off = a & 0xffff;

    switch (off) {
    case 0x00:
        *R(a) = v;
        if (v & (1 << 3)) {
            i2c_ipd(base, 1 << 4);
        }
        if (v & (1 << 4)) {
            i2c_ipd(base, 1 << 5);
        }
        return true;
    case 0x04: case 0x08: case 0x0c: case 0x18:
        return wr_plain(a, old, v, size);
    case 0x1c:
        *R(a) = old & ~v;
        return true;
    case 0x10: {                    /* MTXCNT: transmit from TXDATA */
        uint32_t n = v & 0x3f, i;
        uint8_t b[32];

        *R(a) = v;
        for (i = 0; i < 32; i++) {
            b[i] = *R(base + 0x100 + (i & ~3)) >> ((i & 3) * 8);
        }
        if (n == 0 || !i2c_dev_present(bus, b[0] >> 1)) {
            trace_note("I2C%d write to 0x%02x: no device, NAK", bus, b[0] >> 1);
            i2c_ipd(base, 1 << 6);
            return true;
        }
        if (n >= 2) {
            i2c_ptr[bus] = b[1];
            for (i = 2; i < n; i++) {
                rk817_write(i2c_ptr[bus]++, b[i]);
            }
        }
        i2c_ipd(base, 1 << 2);
        return true;
    }
    case 0x14: {                    /* MRXCNT: receive into RXDATA */
        uint32_t n = v & 0x3f, i, mode = (*R(base) >> 1) & 3;
        uint32_t addr = *R(base + 0x08), raddr = *R(base + 0x0c);
        uint8_t dev = (addr >> 1) & 0x7f;

        *R(a) = v;
        if (!i2c_dev_present(bus, dev)) {
            trace_note("I2C%d read from 0x%02x: no device, NAK", bus, dev);
            i2c_ipd(base, 1 << 6);
            return true;
        }
        if (mode == 1 && (raddr & (1 << 24))) {
            i2c_ptr[bus] = raddr & 0xff;
        }
        for (i = 0; i < 8; i++) {
            *R(base + 0x200 + i * 4) = 0;
        }
        for (i = 0; i < n && i < 32; i++) {
            *R(base + 0x200 + (i & ~3)) |= (uint32_t)rk817[i2c_ptr[bus]++] << ((i & 3) * 8);
        }
        i2c_ipd(base, 1 << 3);
        return true;
    }
    }
    if (off >= 0x100 && off < 0x120) {
        return wr_plain(a, old, v, size);
    }
    return false;
}

static bool i2c_rd(hwaddr a, uint32_t *v)
{
    uint32_t off = a & 0xffff;

    if (off <= 0x1c || (off >= 0x100 && off < 0x120) || (off >= 0x200 && off < 0x220)) {
        return true;
    }
    return false;
}

/* --- SARADC (kernel drivers/iio/adc/rockchip_saradc.c, U-Boot
 * drivers/adc/rockchip-saradc.c): DATA 0x00, STAS 0x04 (bit0 busy), CTRL
 * 0x08 (CHN 2:0, POWER_CTRL bit3, IRQ_ENABLE bit5, IRQ_STATUS bit6 = end of
 * conversion), DLY_PU_SOC 0x0c. Powering up converts the selected channel at
 * once. Channel values from adc= (default ch0 = 166, inside U-Boot hwrev's
 * "rg351mp" window 146..186 that the R36S family reads). */
static bool saradc_rd(hwaddr a, uint32_t *v)
{
    switch (a & 0xff) {
    case 0x00: case 0x08: case 0x0c:
        return true;
    case 0x04:
        *v = 0;
        return true;
    }
    return false;
}

static bool saradc_wr(hwaddr a, uint32_t old, uint32_t v, unsigned size)
{
    hwaddr base = a & ~0xffULL;

    switch (a & 0xff) {
    case 0x08:
        if (v & (1 << 3)) {
            *R(base) = opt.adc[v & 7] & 0x3ff;
            v |= 1 << 6;
        }
        *R(a) = v;
        return true;
    case 0x0c:
        return wr_plain(a, old, v, size);
    case 0x00: case 0x04:
        return true;
    }
    return false;
}

/* --- CRU / PMUCRU (kernel drivers/clk/rockchip/clk-px30.c, clk-pll.c
 * rk3036 PLLs, clk.h PX30_* offsets): PLL CON0 and CON1 hiword, CON1 bit10
 * LOCK reads 1 while the PLL is powered up (bit13 PD clear): clk-pll.c
 * rockchip_rk3036_pll_wait_lock polls CON1 for it. PLL CON2 (frac) is a plain
 * register. MODE_CON, CLKSEL, CLKGATE, SOFTRST are hiword. GLB_SRST_FST
 * (0xb8) = 0xfdb9 / GLB_SRST_SND (0xbc) = 0xeca8 reset the SoC (clk.c
 * rockchip_restart_notify). CRU PLLs: APLL 0x00, DPLL 0x20, CPLL 0x40,
 * NPLL 0x60; PMUCRU: GPLL 0x00, PMU_MODE 0x20, CLKSEL 0x40, CLKGATE 0x80,
 * SOFTRST 0xc0. */
static bool cru_pll_con(uint32_t off, bool pmucru, int *con)
{
    uint32_t n = pmucru ? 1 : 4, p;

    for (p = 0; p < n; p++) {
        uint32_t b = p * 0x20;
        if (off >= b && off < b + 0x14) {
            *con = (off - b) / 4;
            return true;
        }
    }
    return false;
}

static bool cru_rd(hwaddr a, uint32_t *v)
{
    bool pmucru = (a & ~0xfffULL) == 0xFF2BC000ULL;
    uint32_t off = a & 0xfff;
    int con;

    if (cru_pll_con(off, pmucru, &con)) {
        if (con == 1 && !(*v & (1 << 13))) {
            *v |= 1 << 10;
        }
        return con <= 4;
    }
    if (pmucru) {
        return off == 0x20 || (off >= 0x40 && off < 0x100);
    }
    return off == 0xa0 || off == 0xb8 || off == 0xbc || (off >= 0x100 && off < 0x400);
}

static bool cru_wr(hwaddr a, uint32_t old, uint32_t v, unsigned size)
{
    bool pmucru = (a & ~0xfffULL) == 0xFF2BC000ULL;
    uint32_t off = a & 0xfff;
    int con;

    if (cru_pll_con(off, pmucru, &con)) {
        if (con == 2) {
            return wr_plain(a, old, v, size);
        }
        if (con <= 1) {
            return wr_hiword(a, old & ~(1u << 10), v, size);
        }
        return false;
    }
    if (pmucru) {
        if (off == 0x20 || (off >= 0x40 && off < 0x100)) {
            return wr_hiword(a, old, v, size);
        }
        return false;
    }
    if ((off == 0xb8 && (v & 0xffff) == 0xfdb9) || (off == 0xbc && (v & 0xffff) == 0xeca8)) {
        trace_note("CRU global soft reset (0x%x = 0x%x)", off, v);
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return true;
    }
    if (off == 0xb8 || off == 0xbc) {
        return wr_plain(a, old, v, size);
    }
    if (off == 0xa0 || (off >= 0x100 && off < 0x400)) {
        return wr_hiword(a, old, v, size);
    }
    return false;
}

/* --- DW-MMC (kernel drivers/mmc/host/dw_mmc.c/.h), no card in the slot:
 * CTRL reset bits 2:0 and BMOD SWR clear themselves; a command (CMD bit31)
 * is taken at once: clock updates (bit21) end there, a command without a
 * response (RESP_EXP bit6 clear) ends with CMD_DONE, one expecting a response
 * with CMD_DONE + RTO (nobody answers) in RINTSTS (W1C); MINTSTS = RINTSTS &
 * INTMASK; STATUS = FIFO empty, TX watermark, idle; CDETECT bit0 = 1 (no
 * card, active low card-detect pin). VERID 0x270a (Synopsys 2.70a as on
 * other Rockchip SoCs - assumed), HCON: 32-bit data, internal DMAC. */
static bool dwmmc_rd(hwaddr a, uint32_t *v)
{
    hwaddr base = a & ~0xffffULL;

    switch (a & 0xffff) {
    case 0x040:
        *v = *R(base + 0x044) & *R(base + 0x024);
        return true;
    case 0x048:
        *v = (1 << 1) | (1 << 2);
        return true;
    case 0x050:
        *v = 1;
        return true;
    case 0x06c:
        *v = 0x5342270a;
        return true;
    case 0x070:
        *v = 1 << 7;
        return true;
    case 0x000: case 0x004: case 0x008: case 0x00c: case 0x010: case 0x014:
    case 0x018: case 0x01c: case 0x020: case 0x024: case 0x028: case 0x02c:
    case 0x030: case 0x034: case 0x038: case 0x03c: case 0x044: case 0x04c:
    case 0x058: case 0x064: case 0x068: case 0x074: case 0x078: case 0x080:
    case 0x088: case 0x08c: case 0x090: case 0x100:
        return true;
    }
    return false;
}

static bool dwmmc_wr(hwaddr a, uint32_t old, uint32_t v, unsigned size)
{
    hwaddr base = a & ~0xffffULL;

    switch (a & 0xffff) {
    case 0x000:
        *R(a) = v & ~7u;
        return true;
    case 0x080:
        *R(a) = v & ~1u;
        return true;
    case 0x02c:
        *R(a) = v & ~(1u << 31);
        if ((v & (1u << 31)) && !(v & (1u << 21))) {
            *R(base + 0x044) |= (1 << 2) | ((v & (1 << 6)) ? (1 << 8) : 0);
            *R(base + 0x030) = 0;
        }
        return true;
    case 0x044:
        *R(a) = old & ~v;
        return true;
    case 0x08c:                     /* IDSTS, W1C */
        *R(a) = old & ~v;
        return true;
    case 0x004: case 0x008: case 0x00c: case 0x010: case 0x014: case 0x018:
    case 0x01c: case 0x020: case 0x024: case 0x028: case 0x04c: case 0x058:
    case 0x064: case 0x068: case 0x074: case 0x078: case 0x088: case 0x090:
        return wr_plain(a, old, v, size);
    }
    return false;
}

/* --- DW-MIPI-DSI (kernel drivers/gpu/drm/rockchip/dw-mipi-dsi.c): the
 * generic packet FIFOs are always drained (CMD_PKT_STATUS 0x74 = CMD empty,
 * write payload empty, read payload empty), the PHY is locked with every lane
 * in stop state (PHY_STATUS 0xb0: LOCK bit0, clock lane bit2, data lanes
 * 4/7/9/11). Panel commands (GEN_HDR 0x6c) go to the trace as notes. */
static bool dsi_rd(hwaddr a, uint32_t *v)
{
    switch (a & 0xffff) {
    case 0x74:
        *v = 0x15;
        return true;
    case 0xb0:
        *v = 0x1 | 0x4 | 0x10 | 0x80 | 0x200 | 0x800;
        return true;
    }
    return (a & 0xffff) < 0x100;
}

static bool dsi_wr(hwaddr a, uint32_t old, uint32_t v, unsigned size)
{
    if ((a & 0xffff) == 0x6c) {
        trace_note("DSI GEN_HDR 0x%08x (type 0x%02x)", v, v & 0x3f);
    }
    if ((a & 0xffff) < 0x100) {
        return wr_plain(a, old, v, size);
    }
    return false;
}

static const Rule rules[] = {
    { 0xFF000000, 0x1000,  "pmu",        pmu_rd,    pmu_wr },
    { 0xFF010000, 0x1000,  "pmugrf",     rd_plain,  pmugrf_wr },
    { 0xFF040000, 0x100,   "gpio0",      gpio_rd,   gpio_wr },
    /* GRF: iomux, pulls, drive, IO_VSEL, SOC/PD_VO/MAC con - all hiword
     * (pinctrl-rockchip.c px30 grf offsets, io-domain.c px30, rockchip DSI
     * and VOP grf writes) */
    { 0xFF140000, 0x1000,  "grf",        rd_plain,  wr_hiword },
    { 0xFF160000, 0x100,   "uart2",      uart_rd,   NULL },
    { 0xFF180000, 0x1000,  "i2c0",       i2c_rd,    i2c_wr },
    { 0xFF190000, 0x1000,  "i2c1",       i2c_rd,    i2c_wr },
    { 0xFF1A0000, 0x1000,  "i2c2",       i2c_rd,    i2c_wr },
    { 0xFF1B0000, 0x1000,  "i2c3",       i2c_rd,    i2c_wr },
    /* PWM (pwm-rockchip.c v2 regs: period/duty/ctrl), plain registers */
    { 0xFF200000, 0x40,    "pwm0-3",     rd_plain,  wr_plain },
    { 0xFF208000, 0x40,    "pwm4-7",     rd_plain,  wr_plain },
    { 0xFF250000, 0x100,   "gpio1",      gpio_rd,   gpio_wr },
    { 0xFF260000, 0x100,   "gpio2",      gpio_rd,   gpio_wr },
    { 0xFF270000, 0x100,   "gpio3",      gpio_rd,   gpio_wr },
    { 0xFF288000, 0x100,   "saradc",     saradc_rd, saradc_wr },
    { 0xFF2B0000, 0x1000,  "cru",        cru_rd,    cru_wr },
    { 0xFF2BC000, 0x1000,  "pmucru",     cru_rd,    cru_wr },
    /* USB2 PHY GRF (phy-rockchip-inno-usb2.c property_enable: hiword); the
     * status bits (linestate, bvalid, id) read 0 = nothing attached */
    { 0xFF2C0000, 0x8000,  "usb2phy-grf", rd_plain, wr_hiword },
    /* MIPI video PHY (phy-rockchip-inno-video-combo-phy.c: plain registers,
     * nothing polled) */
    { 0xFF2E0000, 0x10000, "video-phy",  rd_plain,  wr_plain },
    { 0xFF370000, 0x4000,  "sdmmc",      dwmmc_rd,  dwmmc_wr },
    { 0xFF390000, 0x4000,  "emmc",       dwmmc_rd,  dwmmc_wr },
    { 0xFF450000, 0x10000, "dsi",        dsi_rd,    dsi_wr },
    /* VOP big/little (rockchip_vop_reg.c px30 RK3366_LIT layout): plain
     * registers; the state U-Boot leaves comes from regs-init */
    { 0xFF460000, 0x1000,  "vopb",       rd_plain,  wr_plain },
    { 0xFF470000, 0x1000,  "vopl",       rd_plain,  wr_plain },
};

static const char *block_name(hwaddr a)
{
    static const struct { hwaddr base, size; const char *name; } other[] = {
        { 0xFF130000, 0x10000, "gic" },
        { 0xFF1E0000, 0x1000, "wdt" },
        { 0xFF210000, 0x1000, "rktimer" },
        { 0xFF280000, 0x1000, "tsadc" },
        { 0xFF290000, 0x4000, "otp" },
        { 0xFF300000, 0x40000, "usb-otg" },
        { 0xFF340000, 0x20000, "usb-host" },
        { 0xFF3A0000, 0x4000, "sfc" },
        { 0xFF3B0000, 0x4000, "nandc" },
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(rules); i++) {
        if (a >= rules[i].base && a < rules[i].base + rules[i].size) {
            return rules[i].name;
        }
    }
    for (i = 0; i < ARRAY_SIZE(other); i++) {
        if (a >= other[i].base && a < other[i].base + other[i].size) {
            return other[i].name;
        }
    }
    return "?";
}

static const Rule *find_rule(hwaddr a)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(rules); i++) {
        if (a >= rules[i].base && a < rules[i].base + rules[i].size) {
            return &rules[i];
        }
    }
    return NULL;
}

static uint64_t regs_read(void *opaque, hwaddr off, unsigned size)
{
    hwaddr a = REGS_BASE + off, wa = a & ~3ULL;
    uint32_t v = *R(wa);
    const Rule *r = find_rule(wa);
    bool handled = r && r->rd && r->rd(wa, &v);
    uint64_t ret = v;

    if (size < 4) {
        ret = (v >> ((a & 3) * 8)) & ((1ULL << (size * 8)) - 1);
    }
    trace_access('r', a, size, ret, handled);
    return ret;
}

static void regs_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    hwaddr a = REGS_BASE + off, wa = a & ~3ULL;
    uint32_t old = *R(wa), v = val;
    const Rule *r = find_rule(wa);
    bool handled;

    if (size < 4) {
        uint32_t sh = (a & 3) * 8, m = (uint32_t)(((1ULL << (size * 8)) - 1) << sh);
        v = (old & ~m) | (((uint32_t)val << sh) & m);
    }
    handled = r && r->wr && r->wr(wa, old, v, size);
    if (!handled) {
        *R(wa) = v;
    }
    trace_access('w', a, size, val, handled);
}

static const MemoryRegionOps regs_ops = {
    .read = regs_read,
    .write = regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */

static void regs_open(void)
{
    if (opt.regs_path) {
        int fd = open(opt.regs_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || ftruncate(fd, REGS_SIZE) != 0) {
            error_report("andr36oid-px30: cannot create %s: %s", opt.regs_path, strerror(errno));
            exit(1);
        }
        regs = mmap(NULL, REGS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (regs == MAP_FAILED) {
            error_report("andr36oid-px30: mmap %s: %s", opt.regs_path, strerror(errno));
            exit(1);
        }
    } else {
        regs = g_malloc0(REGS_SIZE);
    }
}

static void regs_apply_init(void)
{
    FILE *f;
    char line[256];
    unsigned n = 0;

    memset(regs, 0, REGS_SIZE);
    rk817_reset();
    memset(i2c_ptr, 0, sizeof(i2c_ptr));
    if (!opt.regs_init) {
        return;
    }
    f = fopen(opt.regs_init, "r");
    if (!f) {
        error_report("andr36oid-px30: cannot read %s", opt.regs_init);
        exit(1);
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned long long addr, val;
        char *p = line;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == 0) {
            continue;
        }
        if (sscanf(p, "%llx %llx", &addr, &val) != 2 || addr < REGS_BASE ||
            addr >= REGS_BASE + REGS_SIZE || (addr & 3)) {
            error_report("andr36oid-px30: bad line in %s: %s", opt.regs_init, line);
            exit(1);
        }
        *R(addr) = (uint32_t)val;
        n++;
    }
    fclose(f);
    if (trace_fp) {
        fprintf(trace_fp, "# regs-init: %u registers from %s\n", n, opt.regs_init);
    }
}

static void px30_reset(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    int i;

    regs_apply_init();
    cpu_reset(CPU(cpu));
    for (i = 0; i < 4; i++) {
        env->xregs[i] = opt.x[i];
    }
    env->pc = opt.entry;
    if (trace_fp) {
        fprintf(trace_fp, "# reset: pc=%010" PRIx64 " x0=%" PRIx64 " x1=%" PRIx64
                " x2=%" PRIx64 " x3=%" PRIx64 " EL%d\n", env->pc, env->xregs[0],
                env->xregs[1], env->xregs[2], env->xregs[3], (int)((env->pstate >> 2) & 3));
        fflush(trace_fp);
    }
}

static Notifier exit_notifier;

static void px30_exit(Notifier *n, void *data)
{
    if (trace_fp) {
        trace_flush_repeat();
        if (the_cpu) {
            CPUARMState *env = &the_cpu->env;
            fprintf(trace_fp, "# exit: pc=%010" PRIx64 " lr=%010" PRIx64 " sp=%010" PRIx64
                    " EL%d, %" PRIu64 " traced accesses\n", env->pc, env->xregs[30],
                    env->xregs[31], (int)((env->pstate >> 2) & 3), trace_seq);
        }
        fclose(trace_fp);
        trace_fp = NULL;
    }
    if (regs && opt.regs_path) {
        msync(regs, REGS_SIZE, MS_ASYNC);
    }
}

static void px30_init(MachineState *ms)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *regs_mr = g_new0(MemoryRegion, 1);
    Object *cpuobj;
    DeviceState *cpudev, *gic;
    SysBusDevice *gicsbd;
    int ppi = NUM_IRQ - 32 + 16;     /* GIC input of PPI 0 (INTID 16) for CPU 0 */

    if (ms->ram_size > 4 * GiB - 16 * MiB) {
        error_report("andr36oid-px30: at most 4080 MB of RAM");
        exit(1);
    }
    memory_region_add_subregion(sysmem, 0, ms->ram);

    if (opt.trace_path) {
        trace_fp = fopen(opt.trace_path, "w");
        if (!trace_fp) {
            error_report("andr36oid-px30: cannot write %s", opt.trace_path);
            exit(1);
        }
        setvbuf(trace_fp, NULL, _IOFBF, 1 << 20);
        fprintf(trace_fp, "# andr36oid-px30 register trace: <r|w><size> <addr> <value> pc= lr= <block> [UNHANDLED]\n");
    }
    regs_open();

    cpuobj = object_new(ms->cpu_type);
    object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
    object_property_set_bool(cpuobj, "has_el2", true, &error_fatal);
    object_property_set_uint(cpuobj, "psci-conduit", QEMU_PSCI_CONDUIT_SMC, &error_fatal);
    object_property_set_uint(cpuobj, "rvbar", opt.entry, &error_fatal);
    object_property_set_uint(cpuobj, "cntfrq", 24000000, &error_fatal);
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
    cpudev = DEVICE(cpuobj);
    the_cpu = ARM_CPU(cpuobj);

    gic = qdev_new(TYPE_ARM_GIC);
    qdev_prop_set_uint32(gic, "revision", 2);
    qdev_prop_set_uint32(gic, "num-cpu", 1);
    qdev_prop_set_uint32(gic, "num-irq", NUM_IRQ);
    qdev_prop_set_bit(gic, "has-security-extensions", false);
    gicsbd = SYS_BUS_DEVICE(gic);
    sysbus_realize_and_unref(gicsbd, &error_fatal);
    sysbus_mmio_map(gicsbd, 0, 0xFF131000);
    sysbus_mmio_map(gicsbd, 1, 0xFF132000);
    sysbus_connect_irq(gicsbd, 0, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
    sysbus_connect_irq(gicsbd, 1, qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    sysbus_connect_irq(gicsbd, 2, qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
    sysbus_connect_irq(gicsbd, 3, qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    qdev_connect_gpio_out(cpudev, GTIMER_PHYS, qdev_get_gpio_in(gic, ppi + 14));
    qdev_connect_gpio_out(cpudev, GTIMER_VIRT, qdev_get_gpio_in(gic, ppi + 11));
    qdev_connect_gpio_out(cpudev, GTIMER_HYP, qdev_get_gpio_in(gic, ppi + 10));
    qdev_connect_gpio_out(cpudev, GTIMER_SEC, qdev_get_gpio_in(gic, ppi + 13));

    serial_mm_init(sysmem, 0xFF160000, 2, qdev_get_gpio_in(gic, 17), 24000000 / 16,
                   serial_hd(0), DEVICE_LITTLE_ENDIAN);

    if (opt.usb) {
        DeviceState *usb = qdev_new("dwc2-usb");
        object_property_add_const_link(OBJECT(usb), "dma-mr", OBJECT(sysmem));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(usb), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(usb), 0, 0xFF300000);
        sysbus_connect_irq(SYS_BUS_DEVICE(usb), 0, qdev_get_gpio_in(gic, 62));
    }

    memory_region_init_io(regs_mr, NULL, &regs_ops, NULL, "px30-regs", REGS_SIZE);
    memory_region_add_subregion_overlap(sysmem, REGS_BASE, regs_mr, -1);

    qemu_register_reset(px30_reset, the_cpu);
    exit_notifier.notify = px30_exit;
    qemu_add_exit_notifier(&exit_notifier);
}

/* ------------------------------------------------------------------------ */
/* Options as machine properties                                             */

static bool parse_u64(const char *s, uint64_t *out, Error **errp)
{
    if (qemu_strtou64(s, NULL, 0, out) < 0) {
        error_setg(errp, "bad number '%s'", s);
        return false;
    }
    return true;
}

static bool parse_list(const char *s, uint32_t *tab, unsigned n, Error **errp)
{
    g_auto(GStrv) items = g_strsplit(s, ":", -1);
    int i;

    for (i = 0; items[i]; i++) {
        uint64_t k, v;
        char *eq = strchr(items[i], '=');

        if (!eq) {
            error_setg(errp, "expected <index>=<value>, got '%s'", items[i]);
            return false;
        }
        *eq = 0;
        if (!parse_u64(items[i], &k, errp) || !parse_u64(eq + 1, &v, errp)) {
            return false;
        }
        if (k >= n) {
            error_setg(errp, "index %" PRIu64 " out of range", k);
            return false;
        }
        tab[k] = v;
    }
    return true;
}

#define STR_PROP(fn, field)                                                  \
    static char *get_##fn(Object *o, Error **e) { return g_strdup(opt.field ? opt.field : ""); } \
    static void set_##fn(Object *o, const char *v, Error **e) { g_free(opt.field); opt.field = g_strdup(v); }
STR_PROP(regs, regs_path)
STR_PROP(regs_init, regs_init)
STR_PROP(trace, trace_path)

#define U64_PROP(fn, field)                                                  \
    static char *get_##fn(Object *o, Error **e) { return g_strdup_printf("0x%" PRIx64, opt.field); } \
    static void set_##fn(Object *o, const char *v, Error **e) { parse_u64(v, &opt.field, e); }
U64_PROP(entry, entry)
U64_PROP(x0, x[0])
U64_PROP(x1, x[1])
U64_PROP(x2, x[2])
U64_PROP(x3, x[3])

static char *get_adc(Object *o, Error **e) { return g_strdup("(write-only)"); }
static void set_adc(Object *o, const char *v, Error **e) { parse_list(v, opt.adc, 8, e); }
static char *get_gpio_in(Object *o, Error **e) { return g_strdup("(write-only)"); }
static void set_gpio_in(Object *o, const char *v, Error **e) { parse_list(v, opt.gpio_in, 4, e); }

#define BOOL_PROP(fn, field)                                                 \
    static bool get_##fn(Object *o, Error **e) { return opt.field; }          \
    static void set_##fn(Object *o, bool v, Error **e) { opt.field = v; }
BOOL_PROP(trace_all, trace_all)
BOOL_PROP(charger, charger)
BOOL_PROP(usb, usb)

static void px30_machine_init(MachineClass *mc)
{
    ObjectClass *oc = OBJECT_CLASS(mc);

    mc->desc = "andr36oid PX30/RK3326 handheld (UEFI bring-up model)";
    mc->init = px30_init;
    mc->max_cpus = 1;
    mc->default_cpus = 1;
    mc->default_ram_size = 1 * GiB;
    mc->default_ram_id = "andr36oid-px30.ram";
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a35");
    mc->no_cdrom = 1;
    mc->no_floppy = 1;
    mc->no_parallel = 1;

    object_class_property_add_str(oc, "regs", get_regs, set_regs);
    object_class_property_add_str(oc, "regs-init", get_regs_init, set_regs_init);
    object_class_property_add_str(oc, "trace", get_trace, set_trace);
    object_class_property_add_str(oc, "entry", get_entry, set_entry);
    object_class_property_add_str(oc, "x0", get_x0, set_x0);
    object_class_property_add_str(oc, "x1", get_x1, set_x1);
    object_class_property_add_str(oc, "x2", get_x2, set_x2);
    object_class_property_add_str(oc, "x3", get_x3, set_x3);
    object_class_property_add_str(oc, "adc", get_adc, set_adc);
    object_class_property_add_str(oc, "gpio-in", get_gpio_in, set_gpio_in);
    object_class_property_add_bool(oc, "trace-all", get_trace_all, set_trace_all);
    object_class_property_add_bool(oc, "charger", get_charger, set_charger);
    object_class_property_add_bool(oc, "dwc2", get_usb, set_usb);
}

DEFINE_MACHINE_AARCH64("andr36oid-px30", px30_machine_init)

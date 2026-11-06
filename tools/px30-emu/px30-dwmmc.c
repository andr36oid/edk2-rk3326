/*
 * px30-dwmmc: the Synopsys DesignWare MMC host (DW_mmc 2.70a) as the PX30 has
 * it at 0xFF370000 (SD slot, SPI 54) and 0xFF390000 (eMMC, SPI 53), on QEMU's
 * SD bus, so QEMU's sd-card/emmc models answer the commands.
 *
 * Copied into QEMU's hw/arm/ by qemu-build.sh next to andr36oid-px30.c.
 * The behaviour comes from the drivers that run on the real controller, never
 * from the firmware under test:
 *   Linux 4.19 (andr36oid) drivers/mmc/host/dw_mmc.c, dw_mmc.h, dw_mmc-rockchip.c
 *   U-Boot 2017.09 (odroidgoa) drivers/mmc/dw_mmc.c, include/dwmmc.h
 *
 * What is modelled:
 *   - registers 0x00-0x98, 0x100 (CDTHRCTL), 0x104, 0x108 (UHS_REG_EXT) with
 *     reset values; VERID 0x5342270a, so the FIFO window is at 0x200
 *     (dw_mmc.h DATA_240A_OFFSET); HCON: 1 slot, 32-bit host data, internal
 *     DMAC (trans mode 0), 32-bit IDMAC addresses (ADDR_CONFIG 0)
 *   - CMD with START: update-clock-only (bit21) latches CLKDIV/CLKSRC/CLKENA
 *     and clears START without an interrupt (dw_mci mci_send_cmd waits for
 *     START to clear); a command while START is still set raises HLE
 *   - commands run at once on the SD bus; response-less ones end in CMD_DONE,
 *     the rest in CMD_DONE plus the response in RESP0-3 (136-bit: RESP3 =
 *     bits 127:96 incl. the CRC7 byte, as dw_mci_command_complete reads it),
 *     or RTO when the card does not answer
 *   - a command with the card clock off (never latched by an update-clock
 *     command, or CLKENA bit0 clear) gets no response (RTO) and a note
 *   - data (bit9): BYTCNT bytes, read or write (bit10), through the IDMAC
 *     when CTRL.USE_IDMAC (bit25) and BMOD.DE (bit7) are set, else through
 *     the 256-word FIFO by hand (RXDR above RX_WMark, TXDR at/below
 *     TX_WMark, FIFO count in STATUS 29:17); DATA_OVER when the card side is
 *     done; auto-stop (SEND_STOP bit12) sends CMD12 after the data, its
 *     response goes to RESP1 and ACD is raised
 *   - IDMAC 32-bit descriptors (16 bytes: des0 flags OWN/CES/ER/CH/FD/LD/DIC,
 *     des1 sizes 12:0 and 25:13, des2/des3 buffers or next descriptor), chained
 *     (CH) or ring (DSL skip, ER wraps to DBADDR); OWN cleared on hand-back;
 *     a descriptor not owned stops the engine with DU (+AI) until PLDMND;
 *     a bad DMA address gives FBE (+AI); RI/TI (+NI) for descriptors without
 *     DIC; IDSTS W1C, interrupt = IDSTS & IDINTEN
 *   - CTRL reset bits 2:0 and BMOD SWR clear themselves; CTRL bit4 gates the
 *     interrupt line (MINTSTS = RINTSTS & INTMASK)
 *   - CDETECT bit0 = 0 with a card (the pin is active low), WRTPRT bit0 from
 *     the card's read-only flag
 * Faults (fault=<kind>:<n>[:<count>]), counted over block reads (CMD17/18):
 *   dcrc  the n-th read gets its data with one byte flipped and DCRC set
 *   rto   the n-th read command gets no response (RTO)
 *   drto  the n-th read gets a response but no data (DRTO, no DATA_OVER)
 *   dead  from the n-th read on, no command gets a response
 *   count (default 1, 0 = for ever) is how many reads in a row are hit.
 * Every command, response, transfer and fault goes to log=<file> when set.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "system/dma.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"

/* andr36oid-px30.c: clk_sdmmc / clk_emmc from the CRU registers */
uint64_t px30_mmc_clk_hz(bool emmc);

#define TYPE_PX30_DWMMC "px30-dwmmc"
OBJECT_DECLARE_SIMPLE_TYPE(Px30DwMmc, PX30_DWMMC)

#define FIFO_WORDS 256

enum {
    R_CTRL = 0x00, R_PWREN = 0x04, R_CLKDIV = 0x08, R_CLKSRC = 0x0c,
    R_CLKENA = 0x10, R_TMOUT = 0x14, R_CTYPE = 0x18, R_BLKSIZ = 0x1c,
    R_BYTCNT = 0x20, R_INTMASK = 0x24, R_CMDARG = 0x28, R_CMD = 0x2c,
    R_RESP0 = 0x30, R_RESP1 = 0x34, R_RESP2 = 0x38, R_RESP3 = 0x3c,
    R_MINTSTS = 0x40, R_RINTSTS = 0x44, R_STATUS = 0x48, R_FIFOTH = 0x4c,
    R_CDETECT = 0x50, R_WRTPRT = 0x54, R_GPIO = 0x58, R_TCBCNT = 0x5c,
    R_TBBCNT = 0x60, R_DEBNCE = 0x64, R_USRID = 0x68, R_VERID = 0x6c,
    R_HCON = 0x70, R_UHS = 0x74, R_RST_N = 0x78, R_BMOD = 0x80,
    R_PLDMND = 0x84, R_DBADDR = 0x88, R_IDSTS = 0x8c, R_IDINTEN = 0x90,
    R_DSCADDR = 0x94, R_BUFADDR = 0x98, R_CDTHRCTL = 0x100,
    R_UHS_EXT = 0x108, R_DATA = 0x200,
};

/* RINTSTS */
#define INT_RE      (1u << 1)
#define INT_CMD     (1u << 2)
#define INT_DTO     (1u << 3)
#define INT_TXDR    (1u << 4)
#define INT_RXDR    (1u << 5)
#define INT_RCRC    (1u << 6)
#define INT_DCRC    (1u << 7)
#define INT_RTO     (1u << 8)
#define INT_DRTO    (1u << 9)
#define INT_HLE     (1u << 12)
#define INT_ACD     (1u << 14)
/* CMD */
#define CMD_START   (1u << 31)
#define CMD_UPD_CLK (1u << 21)
#define CMD_SEND_STOP (1u << 12)
#define CMD_WR      (1u << 10)
#define CMD_DATA    (1u << 9)
#define CMD_LONG    (1u << 7)
#define CMD_RESP    (1u << 6)
/* IDMAC */
#define DES0_DIC    (1u << 1)
#define DES0_LD     (1u << 2)
#define DES0_FD     (1u << 3)
#define DES0_CH     (1u << 4)
#define DES0_ER     (1u << 5)
#define DES0_OWN    (1u << 31)
#define ID_TI       (1u << 0)
#define ID_RI       (1u << 1)
#define ID_FBE      (1u << 2)
#define ID_DU       (1u << 4)
#define ID_NI       (1u << 8)
#define ID_AI       (1u << 9)

enum { F_NONE, F_DCRC, F_RTO, F_DRTO, F_DEAD };

struct Px30DwMmc {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    SDBus sdbus;
    qemu_irq irq;
    char *name;
    char *fault;
    char *log_path;
    FILE *log;

    uint32_t reg[0x110 / 4];
    bool clk_on;            /* latched by an update-clock command */
    uint32_t clkdiv_latched;
    bool emmc;
    uint64_t card_hz;       /* card clock at the last update-clock command */
    bool ident;             /* between CMD0 and the first CMD3/CMD7 */
    bool hs;                /* the card was switched to high speed */
    uint32_t clk_warnings;

    /* the data phase of the current command */
    bool xfer;              /* a data phase is running */
    bool xfer_write;
    bool xfer_dma;
    bool dma_waiting;       /* IDMAC stopped on a descriptor it does not own */
    uint32_t card_left;     /* bytes still to move between FIFO/DMA and card */
    uint32_t host_left;     /* bytes still to move between host and FIFO (PIO) */
    uint32_t cmd;           /* the command that started the data phase */
    uint32_t dma_desc;      /* next descriptor (IDMAC) */
    bool corrupt;           /* dcrc fault on this transfer */
    uint8_t fifo[FIFO_WORDS * 4];
    uint32_t fifo_rd, fifo_n;   /* bytes */

    int fault_kind;
    uint32_t fault_at, fault_count, reads;
};

#define REG(s, r) ((s)->reg[(r) / 4])

static void dw_log(Px30DwMmc *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void dw_log(Px30DwMmc *s, const char *fmt, ...)
{
    va_list ap;

    if (!s->log) {
        return;
    }
    fprintf(s->log, "%s: ", s->name);
    va_start(ap, fmt);
    vfprintf(s->log, fmt, ap);
    va_end(ap);
    fputc('\n', s->log);
    fflush(s->log);
}

static void dw_update_irq(Px30DwMmc *s)
{
    bool lvl = ((REG(s, R_CTRL) & (1u << 4)) && (REG(s, R_RINTSTS) & REG(s, R_INTMASK))) ||
               (REG(s, R_IDSTS) & REG(s, R_IDINTEN));

    qemu_set_irq(s->irq, lvl);
}

static uint32_t rx_wmark(Px30DwMmc *s)
{
    return (REG(s, R_FIFOTH) >> 16) & 0xfff;
}

static uint32_t tx_wmark(Px30DwMmc *s)
{
    return REG(s, R_FIFOTH) & 0xfff;
}

static void fifo_reset(Px30DwMmc *s)
{
    s->fifo_rd = s->fifo_n = 0;
}

static bool fault_hits(Px30DwMmc *s, int kind)
{
    if (s->fault_kind != kind || s->reads < s->fault_at) {
        return false;
    }
    return s->fault_count == 0 || s->reads < s->fault_at + s->fault_count;
}

static bool card_dead(Px30DwMmc *s)
{
    return s->fault_kind == F_DEAD && s->reads >= s->fault_at;
}

/* end of the card-side data phase: DATA_OVER, then auto-stop */
static void data_done(Px30DwMmc *s)
{
    uint8_t resp[16];
    SDRequest req = { .cmd = 12, .arg = 0 };

    REG(s, R_RINTSTS) |= INT_DTO;
    if (s->corrupt) {
        REG(s, R_RINTSTS) |= INT_DCRC;
        dw_log(s, "  fault: DCRC on this transfer");
        s->corrupt = false;
    }
    if (s->cmd & CMD_SEND_STOP) {
        size_t n = sdbus_do_command(&s->sdbus, &req, resp, sizeof(resp));

        REG(s, R_RESP1) = n >= 4 ? ldl_be_p(resp) : 0;
        REG(s, R_RINTSTS) |= INT_ACD;
        dw_log(s, "  auto-stop CMD12 -> %s 0x%08x", n ? "resp" : "no response", REG(s, R_RESP1));
    }
    dw_log(s, "  data over (%s, %u bytes)", s->xfer_write ? "write" : "read", REG(s, R_TCBCNT));
    s->xfer = false;
}

static void card_read(Px30DwMmc *s, uint8_t *buf, uint32_t n)
{
    uint32_t done = REG(s, R_TCBCNT);

    if (!sdbus_data_ready(&s->sdbus)) {
        memset(buf, 0xff, n);
        return;
    }
    sdbus_read_data(&s->sdbus, buf, n);
    if (s->corrupt && done == 0) {
        buf[0] ^= 0x5a;     /* the first byte of the first block */
    }
}

/* PIO read: keep the FIFO as full as the card allows */
static void fifo_fill(Px30DwMmc *s)
{
    while (s->xfer && !s->xfer_write && !s->xfer_dma && s->card_left &&
           s->fifo_n + 4 <= sizeof(s->fifo)) {
        uint32_t n = MIN(4, s->card_left), w = (s->fifo_rd + s->fifo_n) % sizeof(s->fifo);
        uint8_t b[4] = { 0 };

        if (!sdbus_data_ready(&s->sdbus)) {
            REG(s, R_RINTSTS) |= INT_DRTO;
            dw_log(s, "  card stopped sending with %u bytes left: DRTO", s->card_left);
            s->xfer = false;
            break;
        }
        card_read(s, b, n);
        memcpy(&s->fifo[w], b, 4);
        s->fifo_n += 4;
        s->card_left -= n;
        REG(s, R_TCBCNT) += n;
        if (!s->card_left) {
            data_done(s);
        }
    }
    if (s->fifo_n / 4 > rx_wmark(s)) {
        REG(s, R_RINTSTS) |= INT_RXDR;
    }
}

static void pio_tx_status(Px30DwMmc *s)
{
    if (s->xfer && s->xfer_write && !s->xfer_dma && s->host_left &&
        s->fifo_n / 4 <= tx_wmark(s)) {
        REG(s, R_RINTSTS) |= INT_TXDR;
    }
}

/* IDMAC: move as much as the descriptors allow */
static void idmac_run(Px30DwMmc *s)
{
    AddressSpace *as = &address_space_memory;
    uint32_t dsl = (REG(s, R_BMOD) >> 2) & 0x1f;

    s->dma_waiting = false;
    while (s->xfer && s->xfer_dma && s->card_left) {
        uint32_t d[4], i;
        uint32_t addr = s->dma_desc;

        REG(s, R_DSCADDR) = addr;
        if (dma_memory_read(as, addr, d, sizeof(d), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            REG(s, R_IDSTS) |= ID_FBE | ID_AI;
            dw_log(s, "  IDMAC: descriptor at 0x%08x unreadable: FBE", addr);
            s->xfer = false;
            return;
        }
        for (i = 0; i < 4; i++) {
            d[i] = le32_to_cpu(d[i]);
        }
        if (!(d[0] & DES0_OWN)) {
            REG(s, R_IDSTS) |= ID_DU | ID_AI;
            s->dma_waiting = true;
            dw_log(s, "  IDMAC: descriptor 0x%08x not owned (des0 0x%08x): DU, waiting for PLDMND", addr, d[0]);
            return;
        }
        for (i = 0; i < 2 && s->card_left; i++) {
            uint32_t len = i == 0 ? (d[1] & 0x1fff) : ((d[1] >> 13) & 0x1fff);
            uint32_t buf = i == 0 ? d[2] : d[3];
            uint8_t tmp[8192];

            if (i == 1 && (d[0] & DES0_CH)) {
                break;          /* des3 is the next descriptor */
            }
            len = MIN(len, s->card_left);
            if (!len) {
                continue;
            }
            REG(s, R_BUFADDR) = buf;
            if (s->xfer_write) {
                if (dma_memory_read(as, buf, tmp, len, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                    goto bus_error;
                }
                if (!sdbus_receive_ready(&s->sdbus)) {
                    REG(s, R_RINTSTS) |= INT_DRTO;
                    dw_log(s, "  card not receiving with %u bytes left", s->card_left);
                    s->xfer = false;
                    return;
                }
                sdbus_write_data(&s->sdbus, tmp, len);
            } else {
                if (!sdbus_data_ready(&s->sdbus)) {
                    REG(s, R_RINTSTS) |= INT_DRTO;
                    dw_log(s, "  card stopped sending with %u bytes left: DRTO", s->card_left);
                    s->xfer = false;
                    return;
                }
                card_read(s, tmp, len);
                if (dma_memory_write(as, buf, tmp, len, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                    goto bus_error;
                }
            }
            dw_log(s, "  IDMAC %s 0x%08x len %u (desc 0x%08x des0 0x%08x)",
                   s->xfer_write ? "from" : "to", buf, len, addr, d[0]);
            s->card_left -= len;
            REG(s, R_TCBCNT) += len;
            REG(s, R_TBBCNT) += len;
        }
        /* hand the descriptor back */
        d[0] &= ~DES0_OWN;
        {
            uint32_t w = cpu_to_le32(d[0]);
            dma_memory_write(as, addr, &w, 4, MEMTXATTRS_UNSPECIFIED);
        }
        if (!(d[0] & DES0_DIC)) {
            REG(s, R_IDSTS) |= (s->xfer_write ? ID_TI : ID_RI) | ID_NI;
        }
        if (d[0] & DES0_LD) {
            if (s->card_left) {
                dw_log(s, "  IDMAC: last descriptor with %u bytes left", s->card_left);
            }
            break;
        }
        if (d[0] & DES0_CH) {
            s->dma_desc = d[3];
        } else if (d[0] & DES0_ER) {
            s->dma_desc = REG(s, R_DBADDR);
        } else {
            s->dma_desc = addr + 16 + dsl * 4;
        }
    }
    if (s->xfer && !s->card_left) {
        data_done(s);
    } else if (s->xfer && s->card_left) {
        /* LD before BYTCNT was reached: the card waits, the host times out */
        dw_log(s, "  IDMAC ran out of descriptors, %u bytes left", s->card_left);
    }
    return;

bus_error:
    REG(s, R_IDSTS) |= ID_FBE | ID_AI;
    dw_log(s, "  IDMAC: bus error at buffer 0x%08x: FBE", REG(s, R_BUFADDR));
    s->xfer = false;
}

static void start_command(Px30DwMmc *s, uint32_t cmd)
{
    SDRequest req = { .cmd = cmd & 0x3f, .arg = REG(s, R_CMDARG) };
    uint8_t resp[16];
    size_t n = 0;
    bool is_read = (cmd & CMD_DATA) && !(cmd & CMD_WR);
    bool block_read = req.cmd == 17 || req.cmd == 18;

    if (cmd & CMD_UPD_CLK) {
        /* dw_mmc-rockchip.c: the CRU clock goes through a fixed /2
         * (RK3288_CLKGEN_DIV) to the CIU; CLKDIV n > 0 divides by 2n */
        uint64_t ciu = px30_mmc_clk_hz(s->emmc) / 2;
        uint32_t div = REG(s, R_CLKDIV) & 0xff;

        s->clk_on = REG(s, R_CLKENA) & 1;
        s->clkdiv_latched = REG(s, R_CLKDIV);
        s->card_hz = div ? ciu / (2 * div) : ciu;
        dw_log(s, "update clock: CLKENA 0x%x CLKDIV %u CLKSRC 0x%x: CIU %" PRIu64 " Hz, card %" PRIu64 " Hz",
               REG(s, R_CLKENA), div, REG(s, R_CLKSRC), ciu, s->clk_on ? s->card_hz : 0);
        return;
    }
    if (req.cmd == 0) {
        s->ident = true;
        s->hs = false;
    } else if (req.cmd == 7) {
        s->ident = false;
    }
    if (s->clk_on) {
        uint64_t max = s->ident ? 400000 : s->emmc ? (s->hs ? 52000000 : 26000000)
                                                   : (s->hs ? 50000000 : 25000000);
        if (s->card_hz > max) {
            s->clk_warnings++;
            dw_log(s, "  CLOCK: CMD%u at %" PRIu64 " Hz, more than the %" PRIu64 " Hz the card allows%s",
                   req.cmd, s->card_hz, max, s->ident ? " during identification" : "");
        }
    }
    if (block_read && sdbus_get_inserted(&s->sdbus)) {
        s->reads++;
    }

    dw_log(s, "CMD%u arg 0x%08x flags 0x%08x%s%s%s", req.cmd, req.arg, cmd,
           (cmd & CMD_DATA) ? (is_read ? " read" : " write") : "",
           (cmd & CMD_SEND_STOP) ? " auto-stop" : "", (cmd & (1u << 15)) ? " init" : "");

    REG(s, R_RESP0) = REG(s, R_RESP1) = REG(s, R_RESP2) = REG(s, R_RESP3) = 0;
    if (!s->clk_on || !(REG(s, R_CLKENA) & 1)) {
        dw_log(s, "  card clock is off (no update-clock command latched CLKENA bit0): no response");
        goto no_response;
    }
    if (card_dead(s) || (block_read && fault_hits(s, F_RTO))) {
        dw_log(s, "  fault: %s", card_dead(s) ? "card does not answer any more" : "response timeout");
        goto no_response;
    }
    n = sdbus_do_command(&s->sdbus, &req, resp, sizeof(resp));
    if ((cmd & CMD_RESP) && n == 0) {
        dw_log(s, "  no response");
        goto no_response;
    }
    if (n == 16) {
        REG(s, R_RESP3) = ldl_be_p(&resp[0]);
        REG(s, R_RESP2) = ldl_be_p(&resp[4]);
        REG(s, R_RESP1) = ldl_be_p(&resp[8]);
        REG(s, R_RESP0) = ldl_be_p(&resp[12]);
    } else if (n >= 4) {
        REG(s, R_RESP0) = ldl_be_p(&resp[0]);
    }
    if ((cmd & CMD_RESP) && !!(cmd & CMD_LONG) != (n == 16)) {
        dw_log(s, "  response length %zu does not match the long-response flag", n);
    }
    dw_log(s, "  resp %08x %08x %08x %08x", REG(s, R_RESP0), REG(s, R_RESP1),
           REG(s, R_RESP2), REG(s, R_RESP3));
    REG(s, R_RINTSTS) |= INT_CMD;
    /* SD CMD6 mode 1 group 1 = 1 (high speed); eMMC CMD6 write byte 185 */
    if (req.cmd == 6 && !s->emmc && (cmd & CMD_DATA) && (req.arg & 0x8000000f) == 0x80000001) {
        s->hs = true;
    }
    if (req.cmd == 6 && s->emmc && ((req.arg >> 16) & 0xff) == 185 && ((req.arg >> 8) & 0xff)) {
        s->hs = true;
    }

    if (cmd & CMD_DATA) {
        s->xfer = true;
        s->xfer_write = cmd & CMD_WR;
        s->xfer_dma = (REG(s, R_CTRL) & (1u << 25)) && (REG(s, R_BMOD) & (1u << 7));
        s->card_left = REG(s, R_BYTCNT);
        s->host_left = REG(s, R_BYTCNT);
        s->cmd = cmd;
        s->corrupt = is_read && block_read && fault_hits(s, F_DCRC);
        REG(s, R_TCBCNT) = 0;
        REG(s, R_TBBCNT) = 0;
        if (REG(s, R_BLKSIZ) && (REG(s, R_BYTCNT) % REG(s, R_BLKSIZ))) {
            dw_log(s, "  BYTCNT %u is not a multiple of BLKSIZ %u", REG(s, R_BYTCNT), REG(s, R_BLKSIZ));
        }
        dw_log(s, "  data %u bytes by %s", REG(s, R_BYTCNT), s->xfer_dma ? "IDMAC" : "FIFO");
        if (is_read && block_read && fault_hits(s, F_DRTO)) {
            dw_log(s, "  fault: no data (DRTO)");
            REG(s, R_RINTSTS) |= INT_DRTO;
            s->xfer = false;
        } else if (s->xfer_dma) {
            s->dma_desc = REG(s, R_DBADDR);
            idmac_run(s);
        } else if (is_read) {
            fifo_fill(s);
        } else {
            pio_tx_status(s);
        }
    }
    return;

no_response:
    REG(s, R_RINTSTS) |= INT_CMD | ((cmd & CMD_RESP) ? INT_RTO : 0);
    if ((cmd & CMD_DATA) && !(cmd & CMD_RESP)) {
        REG(s, R_RINTSTS) |= INT_DRTO;
    }
}

static uint32_t fifo_pop(Px30DwMmc *s)
{
    uint32_t v = 0;

    if (s->xfer_write || s->fifo_n < 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: FIFO read with %u bytes in it\n", s->name, s->fifo_n);
        dw_log(s, "  FIFO read while empty");
        return 0;
    }
    memcpy(&v, &s->fifo[s->fifo_rd], 4);
    s->fifo_rd = (s->fifo_rd + 4) % sizeof(s->fifo);
    s->fifo_n -= 4;
    REG(s, R_TBBCNT) += 4;
    fifo_fill(s);
    return le32_to_cpu(v);
}

static void fifo_push(Px30DwMmc *s, uint32_t v)
{
    uint8_t b[4];
    uint32_t n;

    if (!s->xfer || !s->xfer_write || s->xfer_dma || !s->host_left) {
        dw_log(s, "  FIFO write with no write transfer running");
        return;
    }
    stl_le_p(b, v);
    n = MIN(4, s->host_left);
    s->host_left -= n;
    REG(s, R_TBBCNT) += 4;
    if (!sdbus_receive_ready(&s->sdbus)) {
        REG(s, R_RINTSTS) |= INT_DRTO;
        dw_log(s, "  card not receiving with %u bytes left", s->card_left);
        s->xfer = false;
        return;
    }
    sdbus_write_data(&s->sdbus, b, n);
    s->card_left -= n;
    REG(s, R_TCBCNT) += n;
    if (!s->card_left) {
        data_done(s);
    } else {
        pio_tx_status(s);
    }
}

static uint32_t status_reg(Px30DwMmc *s)
{
    uint32_t words = s->fifo_n / 4, v = 0;

    v |= (words & 0x1fff) << 17;
    if (!words) {
        v |= 1 << 2;                        /* FIFO empty */
    }
    if (words >= FIFO_WORDS) {
        v |= 1 << 3;                        /* FIFO full */
    }
    if (words > rx_wmark(s)) {
        v |= 1 << 0;
    }
    if (words <= tx_wmark(s)) {
        v |= 1 << 1;
    }
    if (s->xfer) {
        v |= (1 << 9) | (1 << 10);          /* data busy, data state machine busy */
    }
    return v;
}

static uint64_t dw_read(void *opaque, hwaddr off, unsigned size)
{
    Px30DwMmc *s = opaque;

    if (off >= R_DATA) {
        return fifo_pop(s);
    }
    switch (off) {
    case R_MINTSTS:
        return REG(s, R_RINTSTS) & REG(s, R_INTMASK);
    case R_STATUS:
        return status_reg(s);
    case R_CDETECT:
        return sdbus_get_inserted(&s->sdbus) ? 0 : 1;
    case R_WRTPRT:
        return sdbus_get_readonly(&s->sdbus) ? 1 : 0;
    case R_VERID:
        return 0x5342270a;
    case R_HCON:
        return (1 << 7);
    case R_PLDMND:
        return 0;
    }
    if (off < sizeof(s->reg)) {
        return s->reg[off / 4];
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: read 0x%" HWADDR_PRIx "\n", s->name, off);
    return 0;
}

static void dw_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Px30DwMmc *s = opaque;
    uint32_t v = val;

    if (off >= R_DATA) {
        fifo_push(s, v);
        dw_update_irq(s);
        return;
    }
    switch (off) {
    case R_CTRL:
        if (v & (1 << 0)) {
            /* controller reset: the CIU state machines; a running transfer stops */
            s->xfer = false;
            dw_log(s, "controller reset");
        }
        if (v & (1 << 1)) {
            fifo_reset(s);
        }
        if (v & (1 << 2)) {
            s->dma_waiting = false;
        }
        REG(s, R_CTRL) = v & ~7u;
        break;
    case R_CMD:
        if (REG(s, R_CMD) & CMD_START) {
            REG(s, R_RINTSTS) |= INT_HLE;
            dw_log(s, "CMD written while START still set: HLE");
            break;
        }
        REG(s, R_CMD) = v;
        if (v & CMD_START) {
            if ((v & CMD_DATA) && !(v & CMD_UPD_CLK)) {
                fifo_reset(s);
            }
            start_command(s, v);
            REG(s, R_CMD) = v & ~CMD_START;
        }
        break;
    case R_RINTSTS:
        REG(s, R_RINTSTS) &= ~v;
        /* level-like data requests come back while the condition holds */
        if (s->xfer && !s->xfer_dma && !s->xfer_write && s->fifo_n / 4 > rx_wmark(s)) {
            REG(s, R_RINTSTS) |= INT_RXDR;
        }
        pio_tx_status(s);
        break;
    case R_IDSTS:
        REG(s, R_IDSTS) &= ~(v & 0x3ff);
        break;
    case R_BMOD:
        if (v & 1) {
            REG(s, R_IDSTS) = 0;
            REG(s, R_DSCADDR) = REG(s, R_BUFADDR) = 0;
            s->dma_waiting = false;
            dw_log(s, "IDMAC software reset");
        }
        REG(s, R_BMOD) = v & ~1u;
        break;
    case R_PLDMND:
        if (s->dma_waiting) {
            idmac_run(s);
        }
        break;
    case R_MINTSTS: case R_STATUS: case R_CDETECT: case R_WRTPRT: case R_VERID:
    case R_HCON: case R_TCBCNT: case R_TBBCNT: case R_DSCADDR: case R_BUFADDR:
        break;                      /* read-only */
    default:
        if (off < sizeof(s->reg)) {
            s->reg[off / 4] = v;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: write 0x%" HWADDR_PRIx "\n", s->name, off);
        }
    }
    dw_update_irq(s);
}

static const MemoryRegionOps dw_ops = {
    .read = dw_read,
    .write = dw_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void dw_reset(DeviceState *dev)
{
    Px30DwMmc *s = PX30_DWMMC(dev);

    memset(s->reg, 0, sizeof(s->reg));
    /* reset values: Rockchip TRM / dw_mci_init_dma and U-Boot read FIFOTH for
     * the FIFO depth (RX_WMark = depth - 1) */
    REG(s, R_FIFOTH) = (FIFO_WORDS - 1) << 16;
    REG(s, R_TMOUT) = 0xffffff40;
    REG(s, R_DEBNCE) = 0x00ffffff;
    REG(s, R_RST_N) = 1;
    REG(s, R_USRID) = 0x07967797;
    s->clk_on = false;
    s->xfer = false;
    s->dma_waiting = false;
    s->reads = 0;
    s->ident = s->hs = false;
    s->card_hz = 0;
    fifo_reset(s);
}

static void dw_realize(DeviceState *dev, Error **errp)
{
    Px30DwMmc *s = PX30_DWMMC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &dw_ops, s, s->name ?: "dwmmc", 0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    if (s->log_path && *s->log_path) {
        s->log = fopen(s->log_path, "w");
    }
    if (s->fault && *s->fault) {
        g_auto(GStrv) f = g_strsplit(s->fault, ":", 3);
        static const char *const kinds[] = { "", "dcrc", "rto", "drto", "dead" };
        int k;

        for (k = 1; k < ARRAY_SIZE(kinds); k++) {
            if (!strcmp(f[0], kinds[k])) {
                s->fault_kind = k;
            }
        }
        if (!s->fault_kind || !f[1]) {
            error_setg(errp, "fault=%s: want dcrc|rto|drto|dead:<n>[:<count>]", s->fault);
            return;
        }
        s->fault_at = strtoul(f[1], NULL, 0);
        s->fault_count = f[2] ? strtoul(f[2], NULL, 0) : 1;
        dw_log(s, "fault %s from block read %u, %u in a row", kinds[s->fault_kind],
               s->fault_at, s->fault_count);
    }
}

static void dw_init(Object *obj)
{
    Px30DwMmc *s = PX30_DWMMC(obj);

    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, DEVICE(obj), "sd-bus");
}

static const Property dw_props[] = {
    DEFINE_PROP_STRING("name", Px30DwMmc, name),
    DEFINE_PROP_STRING("fault", Px30DwMmc, fault),
    DEFINE_PROP_STRING("log", Px30DwMmc, log_path),
};

static void dw_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = dw_realize;
    device_class_set_legacy_reset(dc, dw_reset);
    device_class_set_props(dc, dw_props);
}

static const TypeInfo dw_info = {
    .name = TYPE_PX30_DWMMC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Px30DwMmc),
    .instance_init = dw_init,
    .class_init = dw_class_init,
};

static void dw_register(void)
{
    type_register_static(&dw_info);
}
type_init(dw_register)

/* Used by andr36oid-px30.c: one controller with its card (or an empty slot) */
DeviceState *px30_dwmmc_create(hwaddr base, qemu_irq irq, const char *name, DriveInfo *di,
                               bool emmc, const char *fault, const char *log);
DeviceState *px30_dwmmc_create(hwaddr base, qemu_irq irq, const char *name, DriveInfo *di,
                               bool emmc, const char *fault, const char *log)
{
    DeviceState *dev = qdev_new(TYPE_PX30_DWMMC);
    Px30DwMmc *s = PX30_DWMMC(dev);

    s->emmc = emmc;
    qdev_prop_set_string(dev, "name", name);
    if (fault) {
        qdev_prop_set_string(dev, "fault", fault);
    }
    if (log) {
        qdev_prop_set_string(dev, "log", log);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq);
    if (di) {
        DeviceState *card = qdev_new(emmc ? TYPE_EMMC : TYPE_SD_CARD);

        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di), &error_fatal);
        qdev_realize_and_unref(card, BUS(&s->sdbus), &error_fatal);
    }
    return dev;
}

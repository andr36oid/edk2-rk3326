// U-Boot's display arithmetic, copied as it is in the andr36oid U-Boot
// tree (/andr36oid/work/u-boot) with the driver structs reduced to
// arguments. Every function names its origin. Nothing here is shared with
// RkDisplayDxe, so the tests compare two independent copies.
//
// SPDX-License-Identifier: GPL-2.0+

#include <stdio.h>
#include <string.h>
#include "uboot_ref.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define KHz  1000
#define MHz  1000000
#define OSC_HZ  (24 * 1000 * 1000)
#define VCO_MAX_HZ  (3200U * 1000000)
#define VCO_MIN_HZ  (800 * 1000000)
#define PX30_VOP_PLL_LIMIT  600000000
#define PSEC_PER_SEC  1000000000000LL
#define DIV_ROUND_UP(n, d)  (((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST(x, d)  (((x) + ((d) / 2)) / (d))
#define do_div(n, base)  ({ u32 __r = (n) % (base); (n) /= (base); __r; })
#define div_u64(a, b)  ((u64)(a) / (b))
#define lldiv(a, b)  ((u64)(a) / (b))
#define max(a, b)  ((a) > (b) ? (a) : (b))
// include/linux/kernel.h
#define abs(x) ({ long ret; if (sizeof (x) == sizeof (long)) { long __x = (x); ret = (__x < 0) ? -__x : __x; } \
                  else { int __x = (x); ret = (__x < 0) ? -__x : __x; } ret; })

#define DRM_MODE_FLAG_PHSYNC    (1 << 0)
#define DRM_MODE_FLAG_NHSYNC    (1 << 1)
#define DRM_MODE_FLAG_PVSYNC    (1 << 2)
#define DRM_MODE_FLAG_NVSYNC    (1 << 3)
#define DRM_MODE_FLAG_PPIXDATA  (1U << 31)
#define DRM_MODE_FLAG_XMIRROR   (1 << 28)
#define DRM_MODE_FLAG_YMIRROR   (1 << 29)
#define DRM_MODE_FLAG_XYMIRROR  (DRM_MODE_FLAG_XMIRROR | DRM_MODE_FLAG_YMIRROR)

// ---- drivers/clk/rockchip/clk_px30.c --------------------------------------

struct pll_rate_table {
  unsigned long rate;
  unsigned int fbdiv, postdiv1, refdiv, postdiv2, dsmpd, frac;
};

#define PX30_PLL_RATE(_rate, _refdiv, _fbdiv, _postdiv1, _postdiv2, _dsmpd, _frac) \
  { .rate = _rate##U, .fbdiv = _fbdiv, .postdiv1 = _postdiv1, .refdiv = _refdiv, \
    .postdiv2 = _postdiv2, .dsmpd = _dsmpd, .frac = _frac, }

static struct pll_rate_table px30_pll_rates[] = {
  /* _mhz, _refdiv, _fbdiv, _postdiv1, _postdiv2, _dsmpd, _frac */
  PX30_PLL_RATE(1200000000, 1, 50, 1, 1, 1, 0),
  PX30_PLL_RATE(1188000000, 2, 99, 1, 1, 1, 0),
  PX30_PLL_RATE(1100000000, 12, 550, 1, 1, 1, 0),
  PX30_PLL_RATE(1008000000, 1, 84, 2, 1, 1, 0),
  PX30_PLL_RATE(1000000000, 6, 500, 2, 1, 1, 0),
  PX30_PLL_RATE(816000000, 1, 68, 2, 1, 1, 0),
  PX30_PLL_RATE(600000000, 1, 75, 3, 1, 1, 0),
};

static struct pll_rate_table auto_table;

static struct pll_rate_table *pll_clk_set_by_auto(u32 drate)
{
  struct pll_rate_table *rate = &auto_table;
  u32 ref_khz = OSC_HZ / KHz, refdiv, fbdiv = 0;
  u32 postdiv1, postdiv2 = 1;
  u32 fref_khz;
  u32 diff_khz, best_diff_khz;
  const u32 max_refdiv = 63, max_fbdiv = 3200, min_fbdiv = 16;
  const u32 max_postdiv1 = 7, max_postdiv2 = 7;
  u32 vco_khz;
  u32 rate_khz = drate / KHz;

  if (!drate) {
    return NULL;
  }

  postdiv1 = DIV_ROUND_UP(VCO_MIN_HZ / 1000, rate_khz);
  if (postdiv1 > max_postdiv1) {
    postdiv2 = DIV_ROUND_UP(postdiv1, max_postdiv1);
    postdiv1 = DIV_ROUND_UP(postdiv1, postdiv2);
  }

  vco_khz = rate_khz * postdiv1 * postdiv2;

  if (vco_khz < (VCO_MIN_HZ / KHz) || vco_khz > (VCO_MAX_HZ / KHz) ||
      postdiv2 > max_postdiv2) {
    return NULL;
  }

  rate->postdiv1 = postdiv1;
  rate->postdiv2 = postdiv2;

  best_diff_khz = vco_khz;
  for (refdiv = 1; refdiv < max_refdiv && best_diff_khz; refdiv++) {
    fref_khz = ref_khz / refdiv;

    fbdiv = vco_khz / fref_khz;
    if ((fbdiv >= max_fbdiv) || (fbdiv <= min_fbdiv))
      continue;
    diff_khz = vco_khz - fbdiv * fref_khz;
    if (fbdiv + 1 < max_fbdiv && diff_khz > fref_khz / 2) {
      fbdiv++;
      diff_khz = fref_khz - diff_khz;
    }

    if (diff_khz >= best_diff_khz)
      continue;

    best_diff_khz = diff_khz;
    rate->refdiv = refdiv;
    rate->fbdiv = fbdiv;
  }

  if (best_diff_khz > 4 * (MHz / KHz)) {
    return NULL;
  }

  return rate;
}

static const struct pll_rate_table *get_pll_settings(unsigned long rate)
{
  unsigned int rate_count = sizeof (px30_pll_rates) / sizeof (px30_pll_rates[0]);
  int i;

  for (i = 0; i < rate_count; i++) {
    if (rate == px30_pll_rates[i].rate)
      return &px30_pll_rates[i];
  }

  return pll_clk_set_by_auto(rate);
}

const struct ref_pll *ref_get_pll_settings (unsigned long rate)
{
  static struct ref_pll out;
  const struct pll_rate_table *t = get_pll_settings (rate);

  if (t == NULL) {
    return NULL;
  }

  out.refdiv = t->refdiv;
  out.fbdiv = t->fbdiv;
  out.postdiv1 = t->postdiv1;
  out.postdiv2 = t->postdiv2;
  return &out;
}

// rkclk_set_pll's vco_hz and output_hz
unsigned long ref_pll_rate (const struct ref_pll *p)
{
  unsigned vco_hz = OSC_HZ / 1000 * p->fbdiv / p->refdiv * 1000;
  return vco_hz / p->postdiv1 / p->postdiv2;
}

// px30_vop_set_clk, DCLK_VOPB: CPLL goes to hz * src_clk_div
int ref_vop_dclk_div (unsigned int hz)
{
  int src_clk_div;

  if (hz < PX30_VOP_PLL_LIMIT) {
    src_clk_div = DIV_ROUND_UP(PX30_VOP_PLL_LIMIT, hz);
    if (src_clk_div % 2)
      src_clk_div = src_clk_div - 1;
  } else {
    src_clk_div = 1;
  }
  return src_clk_div;
}

// ---- drivers/video/drm/rockchip_display.c display_get_timing_from_dts ------

void ref_mode_from_timing (const struct ref_dt_timing *t, struct ref_mode *mode)
{
  int val, flags = 0;
  int hactive = t->hactive, vactive = t->vactive, pixelclock = t->pixelclock;
  int hfront_porch = t->hfront_porch, hback_porch = t->hback_porch, hsync_len = t->hsync_len;
  int vfront_porch = t->vfront_porch, vback_porch = t->vback_porch, vsync_len = t->vsync_len;

  val = t->hsync_active;
  flags |= val ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
  val = t->vsync_active;
  flags |= val ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;
  val = t->pixelclk_active;
  flags |= val ? DRM_MODE_FLAG_PPIXDATA : 0;
  val = t->screen_rotate;
  if (val == DRM_MODE_FLAG_XMIRROR) {
    flags |= DRM_MODE_FLAG_XMIRROR;
  } else if (val == DRM_MODE_FLAG_YMIRROR) {
    flags |= DRM_MODE_FLAG_YMIRROR;
  } else if (val == DRM_MODE_FLAG_XYMIRROR) {
    flags |= DRM_MODE_FLAG_XMIRROR;
    flags |= DRM_MODE_FLAG_YMIRROR;
  }
  mode->hdisplay = hactive;
  mode->hsync_start = mode->hdisplay + hfront_porch;
  mode->hsync_end = mode->hsync_start + hsync_len;
  mode->htotal = mode->hsync_end + hback_porch;
  mode->vdisplay = vactive;
  mode->vsync_start = mode->vdisplay + vfront_porch;
  mode->vsync_end = mode->vsync_start + vsync_len;
  mode->vtotal = mode->vsync_end + vback_porch;
  mode->clock = pixelclock / 1000;
  mode->flags = flags;
}

// ---- drivers/video/drm/dw_mipi_dsi.c ----------------------------------------

// include/drm/drm_mipi_dsi.h
static int mipi_dsi_pixel_format_to_bpp(int fmt)
{
  switch (fmt) {
  case 0: /* MIPI_DSI_FMT_RGB888 */
  case 1: /* MIPI_DSI_FMT_RGB666 */
    return 24;
  case 2: /* MIPI_DSI_FMT_RGB666_PACKED */
    return 18;
  case 3: /* MIPI_DSI_FMT_RGB565 */
    return 16;
  }
  return -22;
}

// dw_mipi_dsi_get_lane_rate, px30 max_bit_rate_per_lane 1 Gbps, no slave
unsigned long ref_lane_rate (const struct ref_mode *mode, int format, int lanes, uint32_t value)
{
  unsigned long max_lane_rate = 1000000000UL;
  unsigned long lane_rate;
  int bpp;
  u64 tmp;

  if (value > 0)
    return value * 1000 * 1000;

  bpp = mipi_dsi_pixel_format_to_bpp(format);
  if (bpp < 0)
    bpp = 24;

  tmp = (u64)mode->clock * 1000 * bpp;
  do_div(tmp, lanes);

  tmp *= 10;
  do_div(tmp, 9);

  if (tmp > max_lane_rate)
    lane_rate = max_lane_rate;
  else
    lane_rate = tmp;

  return lane_rate;
}

// dw_mipi_dsi_get_hcomponent_lbcc
int ref_lbcc (u32 hcomponent, unsigned lane_mbps, int clock)
{
  u32 lbcc;

  lbcc = hcomponent * lane_mbps * 1000 / 8;

  if (!clock)
    return 0;

  return DIV_ROUND_CLOSEST(lbcc, (u32)clock);
}

// dw_mipi_dsi_init
u32 ref_esc_clk_div (unsigned lane_mbps)
{
  return DIV_ROUND_UP(lane_mbps >> 3, 20);
}

// ---- drivers/video/drm/inno_video_combo_phy.c -------------------------------

unsigned long ref_inno_round_rate (unsigned long prate, unsigned long rate, u8 *prediv, u16 *fbdiv)
{
  unsigned long best_freq = 0;
  unsigned long fref, fout;
  u8 min_prediv, max_prediv;
  u8 _prediv, best_prediv = 1;
  u16 _fbdiv, best_fbdiv = 1;
  u32 min_delta = 0xffffffff;

  fref = prate / 2;
  if (rate > 1000000000UL)
    fout = 1000000000UL;
  else
    fout = rate;

  min_prediv = DIV_ROUND_UP(fref, 40000000);
  max_prediv = fref / 5000000;

  for (_prediv = min_prediv; _prediv <= max_prediv; _prediv++) {
    u64 tmp;
    u32 delta;

    tmp = (u64)fout * _prediv;
    do_div(tmp, fref);
    _fbdiv = tmp;

    if (_fbdiv == 15)
      continue;

    if (_fbdiv < 12 || _fbdiv > 511)
      continue;

    tmp = (u64)_fbdiv * fref;
    do_div(tmp, _prediv);

    delta = abs(fout - tmp);
    if (!delta) {
      best_prediv = _prediv;
      best_fbdiv = _fbdiv;
      best_freq = tmp;
      break;
    } else if (delta < min_delta) {
      best_prediv = _prediv;
      best_fbdiv = _fbdiv;
      best_freq = tmp;
      min_delta = delta;
    }
  }

  if (best_freq) {
    *prediv = best_prediv;
    *fbdiv = best_fbdiv;
  }

  return best_freq;
}

struct mipi_dphy_timing {
  unsigned int clkmiss, clkpost, clkpre, clkprepare, clksettle, clktermen, clktrail, clkzero;
  unsigned int dtermen, eot, hsexit, hsprepare, hszero, hssettle, hsskip, hstrail, init, lpx;
  unsigned int taget, tago, tasure, wakeup;
};

static void mipi_dphy_timing_get_default(struct mipi_dphy_timing *timing, unsigned long period)
{
  timing->clkmiss = 0;
  timing->clkpost = 70000 + 52 * period;
  timing->clkpre = 8 * period;
  timing->clkprepare = 65000;
  timing->clksettle = 95000;
  timing->clktermen = 0;
  timing->clktrail = 80000;
  timing->clkzero = 260000;
  timing->dtermen = 0;
  timing->eot = 0;
  timing->hsexit = 120000;
  timing->hsprepare = 65000 + 4 * period;
  timing->hszero = 145000 + 6 * period;
  timing->hssettle = 85000 + 6 * period;
  timing->hsskip = 40000;
  timing->hstrail = max(8 * period, 60000 + 4 * period);
  timing->init = 100000000;
  timing->lpx = 60000;
  timing->taget = 5 * timing->lpx;
  timing->tago = 4 * timing->lpx;
  timing->tasure = 2 * timing->lpx;
  timing->wakeup = 1000000000;
}

// inno_video_phy_mipi_mode_enable, the computed counters
void ref_inno_timings (unsigned long rate, struct ref_phy_timing *out)
{
  struct mipi_dphy_timing gotp;
  const struct {
    unsigned long rate;
    u8 hs_prepare;
    u8 clk_lane_hs_zero;
    u8 data_lane_hs_zero;
    u8 hs_trail;
  } timings[] = {
    { 110000000, 0x20, 0x16, 0x02, 0x22},
    { 150000000, 0x06, 0x16, 0x03, 0x45},
    { 200000000, 0x18, 0x17, 0x04, 0x0b},
    { 250000000, 0x05, 0x17, 0x05, 0x16},
    { 300000000, 0x51, 0x18, 0x06, 0x2c},
    { 400000000, 0x64, 0x19, 0x07, 0x33},
    { 500000000, 0x20, 0x1b, 0x07, 0x4e},
    { 600000000, 0x6a, 0x1d, 0x08, 0x3a},
    { 700000000, 0x3e, 0x1e, 0x08, 0x6a},
    { 800000000, 0x21, 0x1f, 0x09, 0x29},
    {1000000000, 0x09, 0x20, 0x09, 0x27},
  };
  u32 t_txbyteclkhs, t_txclkesc, ui;
  u32 txbyteclkhs, txclkesc, esc_clk_div;
  u32 hs_exit, clk_post, clk_pre, wakeup, lpx, ta_go, ta_sure, ta_wait;
  u32 hs_prepare, hs_trail, clk_lane_hs_zero, data_lane_hs_zero;
  unsigned int i;

  txbyteclkhs = rate / 8;
  t_txbyteclkhs = div_u64(PSEC_PER_SEC, txbyteclkhs);

  esc_clk_div = DIV_ROUND_UP(txbyteclkhs, 20000000);
  txclkesc = txbyteclkhs / esc_clk_div;
  t_txclkesc = div_u64(PSEC_PER_SEC, txclkesc);

  ui = div_u64(PSEC_PER_SEC, rate);

  memset(&gotp, 0, sizeof(gotp));
  mipi_dphy_timing_get_default(&gotp, ui);

  hs_exit = DIV_ROUND_UP(gotp.hsexit, t_txbyteclkhs);
  clk_post = DIV_ROUND_UP(gotp.clkpost, t_txbyteclkhs);
  clk_pre = DIV_ROUND_UP(gotp.clkpre, t_txbyteclkhs);
  lpx = DIV_ROUND_UP(gotp.lpx, t_txbyteclkhs);
  if (lpx >= 2)
    lpx -= 2;
  ta_go = DIV_ROUND_UP(gotp.tago, t_txclkesc);
  ta_sure = DIV_ROUND_UP(gotp.tasure, t_txclkesc);
  ta_wait = DIV_ROUND_UP(gotp.taget, t_txclkesc);

  for (i = 0; i < sizeof (timings) / sizeof (timings[0]); i++)
    if (rate <= timings[i].rate)
      break;

  if (i == sizeof (timings) / sizeof (timings[0]))
    --i;

  hs_prepare = timings[i].hs_prepare;
  hs_trail = timings[i].hs_trail;
  clk_lane_hs_zero = timings[i].clk_lane_hs_zero;
  data_lane_hs_zero = timings[i].data_lane_hs_zero;
  wakeup = 0x3ff;

  out->hs_exit = hs_exit; out->clk_post = clk_post; out->clk_pre = clk_pre; out->lpx = lpx;
  out->ta_go = ta_go; out->ta_sure = ta_sure; out->ta_wait = ta_wait;
  out->hs_prepare = hs_prepare; out->hs_trail = hs_trail; out->wakeup = wakeup;
  out->clk_lane_hs_zero = clk_lane_hs_zero; out->data_lane_hs_zero = data_lane_hs_zero;
}

// ---- drivers/video/drm/rockchip_vop.c ---------------------------------------

// rockchip_vop_init (timing words, polarity) and rockchip_vop_set_plane /
// display_logo (window at 0,0 of w x h, xvir) for a progressive mode
void ref_vop_config (const struct ref_mode *mode, int w, int h, int bpp, struct ref_vop *v)
{
  u16 hsync_len = mode->hsync_end - mode->hsync_start;
  u16 htotal = mode->htotal;
  u16 hact_st = mode->htotal - mode->hsync_start;
  u16 hact_end = hact_st + mode->hdisplay;
  u16 vtotal = mode->vtotal;
  u16 vsync_len = mode->vsync_end - mode->vsync_start;
  u16 vact_st = mode->vtotal - mode->vsync_start;
  u16 vact_end = vact_st + mode->vdisplay;
  u32 val, act_end;
  int dclk_inv;
  u32 dsp_stx, dsp_sty;

  dclk_inv = (mode->flags & DRM_MODE_FLAG_PPIXDATA) ? 0 : 1;
  val = 0x8;
  val |= (mode->flags & DRM_MODE_FLAG_NHSYNC) ? 0 : 1;
  val |= (mode->flags & DRM_MODE_FLAG_NVSYNC) ? 0 : (1 << 1);
  v->pin_pol = val & 0x7;   // mipi_pin_pol is VOP_REG(..., 0x7, 26)
  v->dclk_inv = dclk_inv;

  v->htotal_pw = (htotal << 16) | hsync_len;
  val = hact_st << 16;
  val |= hact_end;
  v->hact_st_end = val;
  val = vact_st << 16;
  val |= vact_end;
  v->vact_st_end = val;
  act_end = vact_end;
  v->vtotal_pw = (vtotal << 16) | vsync_len;
  v->line_flag = (act_end - 3) & 0xfff;

  v->dsp_info = (h - 1) << 16;
  v->dsp_info |= (w - 1) & 0xffff;
  dsp_stx = 0 + mode->htotal - mode->hsync_start;
  dsp_sty = 0 + mode->vtotal - mode->vsync_start;
  v->dsp_st = dsp_sty << 16 | (dsp_stx & 0xffff);
  // display_logo: xvir = ALIGN(src_w * bpp, 32) >> 5
  v->vir = (((w * bpp) + 31) & ~31) >> 5;
}

// rockchip_vop_init dither_down for output P888, then VOP_REG(.., 0x7, 6)
uint32_t ref_dither_down (uint32_t bus_format)
{
  u32 val;

  switch (bus_format) {
  case 0x1017: /* MEDIA_BUS_FMT_RGB565_1X16 */
    val = (1 << 1) | (0 << 2);
    break;
  case 0x1009: /* MEDIA_BUS_FMT_RGB666_1X18 */
  case 0x1015: /* MEDIA_BUS_FMT_RGB666_1X24_CPADHI */
  case 0x1010: /* MEDIA_BUS_FMT_RGB666_1X7X3_SPWG */
  case 0x101b: /* MEDIA_BUS_FMT_RGB666_1X7X3_JEIDA */
    val = (1 << 1) | (1 << 2);
    break;
  default:
    val = 0;
    break;
  }
  val |= 1;        /* PRE_DITHER_DOWN_EN(1), output is not AAAA */
  val |= 0 << 3;   /* DITHER_DOWN_MODE_SEL(ALLEGRO) */
  return val & 0x7;
}

// ---- drivers/pwm/rk_pwm.c rk_pwm_set_config (prescaler 1) -----------------

unsigned long ref_pwm_count (unsigned long freq, unsigned ns)
{
  return lldiv((uint64_t)(freq / 1000) * ns, 1 * 1000000);
}

// ---- drivers/video/pwm_backlight.c ------------------------------------------

unsigned ref_backlight_duty (unsigned period_ns, const uint32_t *cell, int count, int index)
{
  unsigned default_level, max_level, min_level = 0;

  if (cell && count > index) {
    default_level = cell[index];
    max_level = cell[count - 1];
    if (cell[0] > max_level)
      max_level = cell[0];
  } else {
    default_level = index;
    max_level = 255;
  }
  return period_ns * (default_level - min_level) / (max_level - min_level + 1);
}

// ---- drivers/video/drm/rockchip_panel.c + drm_mipi_dsi.c --------------------

// rockchip_panel_send_dsi_cmds -> mipi_dsi_generic_write / dcs_write_buffer
uint8_t ref_cmd_msg_type (uint8_t data_type, unsigned size)
{
  switch (data_type) {
  case 0x03: case 0x13: case 0x23: case 0x29:
    switch (size) {
    case 0: return 0x03;
    case 1: return 0x13;
    case 2: return 0x23;
    default: return 0x29;
    }
  case 0x05: case 0x15: case 0x39:
    switch (size) {
    case 0: return 0;     /* -EINVAL */
    case 1: return 0x05;
    case 2: return 0x15;
    default: return 0x39;
    }
  default:
    return 0;
  }
}

static bool mipi_dsi_packet_format_is_long(u8 type)
{
  return type == 0x29 || type == 0x39;
}

// mipi_dsi_create_packet, then dw_mipi_dsi_transfer's header word
uint32_t ref_packet_header (unsigned channel, u8 type, const u8 *tx, unsigned tx_len)
{
  u8 header[4] = { 0 };

  header[0] = ((channel & 0x3) << 6) | (type & 0x3f);
  if (mipi_dsi_packet_format_is_long(type)) {
    header[1] = (tx_len >> 0) & 0xff;
    header[2] = (tx_len >> 8) & 0xff;
  } else {
    header[1] = (tx_len > 0) ? tx[0] : 0;
    header[2] = (tx_len > 1) ? tx[1] : 0;
  }
  return header[0] | header[1] << 8 | header[2] << 16 | (u32)header[3] << 24;
}

// ---- drivers/video/drm/dw_mipi_dsi.c host_init register values -------------

#define BIT(n)  (1U << (n))
#define MIPI_DSI_MODE_VIDEO_BURST       BIT(1)
#define MIPI_DSI_MODE_VIDEO_SYNC_PULSE  BIT(2)
#define MIPI_DSI_MODE_VIDEO_HFP         BIT(5)
#define MIPI_DSI_MODE_VIDEO_HBP         BIT(6)
#define MIPI_DSI_MODE_EOT_PACKET        BIT(9)

// dw_mipi_dsi_dpi_config
uint32_t ref_dpi_color (int format)
{
  u32 color = 0;

  switch (format) {
  case 0: color = 0x5; break;            /* DPI_COLOR_CODING_24BIT */
  case 1: color = 0x4 | BIT(8); break;   /* 18BIT_2 | EN18_LOOSELY */
  case 2: color = 0x3; break;            /* 18BIT_1 */
  case 3: color = 0x0; break;            /* 16BIT_1 */
  }
  return color;
}

uint32_t ref_dpi_pol (unsigned flags)
{
  u32 val = 0;

  if (flags & DRM_MODE_FLAG_NVSYNC)
    val |= BIT(1);
  if (flags & DRM_MODE_FLAG_NHSYNC)
    val |= BIT(2);
  return val;
}

// dw_mipi_dsi_packet_handler_config
uint32_t ref_pckhdl (unsigned mode_flags)
{
  u32 val = BIT(4) | BIT(3) | BIT(2) | BIT(0);

  if (mode_flags & MIPI_DSI_MODE_EOT_PACKET)
    val &= ~BIT(0);
  return val;
}

// dw_mipi_dsi_video_mode_config
uint32_t ref_vid_mode_cfg (unsigned mode_flags)
{
  u32 val = BIT(11) | BIT(10) | BIT(9) | BIT(8) | BIT(13) | BIT(12);

  if (mode_flags & MIPI_DSI_MODE_VIDEO_HFP)
    val &= ~BIT(13);
  if (mode_flags & MIPI_DSI_MODE_VIDEO_HBP)
    val &= ~BIT(12);
  if (mode_flags & MIPI_DSI_MODE_VIDEO_BURST)
    val |= 0x2;
  else if (mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
    val |= 0x0;
  else
    val |= 0x1;
  return val;
}

// ---- drivers/video/drm/rockchip_panel.c rockchip_panel_parse_cmds ---------

struct rockchip_cmd_header {
  u8 data_type;
  u8 delay_ms;
  u8 payload_length;
} __attribute__ ((packed));

int ref_parse_cmds (const u8 *data, int length, struct ref_cmd *cmds, int max)
{
  int len;
  const u8 *buf;
  const struct rockchip_cmd_header *header;
  int i, cnt = 0;

  /* scan commands */
  cnt = 0;
  buf = data;
  len = length;
  while (len > sizeof(*header)) {
    header = (const struct rockchip_cmd_header *)buf;
    buf += sizeof(*header) + header->payload_length;
    len -= sizeof(*header) + header->payload_length;
    cnt++;
  }

  if (cnt > max)
    return -1;

  buf = data;
  len = length;
  for (i = 0; i < cnt; i++) {
    header = (const struct rockchip_cmd_header *)buf;
    length -= sizeof(*header);
    buf += sizeof(*header);
    cmds[i].type = header->data_type;
    cmds[i].delay_ms = header->delay_ms;
    cmds[i].len = header->payload_length;
    cmds[i].payload = buf;
    buf += header->payload_length;
    length -= header->payload_length;
  }

  return cnt;
}

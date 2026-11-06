// Reference results from U-Boot's own code (uboot_ref.c), for comparing
// RkDisplayDxe's arithmetic against.
//
// SPDX-License-Identifier: GPL-2.0+

#ifndef UBOOT_REF_H_
#define UBOOT_REF_H_

#include <stdint.h>
#include <stdbool.h>

struct ref_pll {
  unsigned refdiv, fbdiv, postdiv1, postdiv2;
};

struct ref_mode {
  int clock;          // kHz
  int hdisplay, hsync_start, hsync_end, htotal;
  int vdisplay, vsync_start, vsync_end, vtotal;
  unsigned flags;
};

struct ref_dt_timing {
  int hactive, vactive, pixelclock, hsync_len, hfront_porch, hback_porch;
  int vsync_len, vfront_porch, vback_porch, hsync_active, vsync_active;
  int pixelclk_active, screen_rotate;
};

struct ref_phy_timing {
  uint32_t hs_exit, clk_post, clk_pre, lpx, ta_go, ta_sure, ta_wait;
  uint32_t hs_prepare, hs_trail, clk_lane_hs_zero, data_lane_hs_zero, wakeup;
};

struct ref_cmd {
  uint8_t type, delay_ms, len;
  const uint8_t *payload;
};

struct ref_vop {
  uint32_t htotal_pw, hact_st_end, vtotal_pw, vact_st_end, line_flag;
  uint32_t pin_pol, dclk_inv, dsp_info, dsp_st, vir;
};

const struct ref_pll *ref_get_pll_settings (unsigned long rate);
unsigned long ref_pll_rate (const struct ref_pll *p);
int ref_vop_dclk_div (unsigned int hz);
void ref_mode_from_timing (const struct ref_dt_timing *t, struct ref_mode *mode);
unsigned long ref_lane_rate (const struct ref_mode *mode, int format, int lanes, uint32_t lane_rate_prop);
unsigned long ref_inno_round_rate (unsigned long prate, unsigned long rate, uint8_t *prediv, uint16_t *fbdiv);
void ref_inno_timings (unsigned long rate, struct ref_phy_timing *t);
int ref_lbcc (uint32_t hcomponent, unsigned lane_mbps, int clock);
uint32_t ref_esc_clk_div (unsigned lane_mbps);
void ref_vop_config (const struct ref_mode *mode, int w, int h, int bpp, struct ref_vop *v);
uint32_t ref_dither_down (uint32_t bus_format);
unsigned long ref_pwm_count (unsigned long freq, unsigned ns);
unsigned ref_backlight_duty (unsigned period_ns, const uint32_t *levels, int count, int index_prop);
uint8_t ref_cmd_msg_type (uint8_t data_type, unsigned len);
uint32_t ref_packet_header (unsigned channel, uint8_t type, const uint8_t *buf, unsigned len);
uint32_t ref_dpi_color (int format);
uint32_t ref_dpi_pol (unsigned flags);
uint32_t ref_pckhdl (unsigned mode_flags);
uint32_t ref_vid_mode_cfg (unsigned mode_flags);
int ref_parse_cmds (const uint8_t *data, int length, struct ref_cmd *cmds, int max);

#endif

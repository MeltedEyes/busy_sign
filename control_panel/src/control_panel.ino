#include <Arduino.h>
#include <FFat.h>                 // MUST precede LovyanGFX — it generates
                                  // the filesystem wrapper from what it sees
#include <lvgl.h>
#include "LovyanGFX_Driver.h"
#include <panel_hw.h>
#include <rtc_bm8563.h>
#include <sign_prefs.h>
#include <sign_config.h>

// =====================================================================
// control_panel — desk unit, master
// Pages 0..n-1 : 3x2 status tile grid, swipe between them
// Page n       : settings
//
// Tap a tile: sends SCR <id> immediately.
// Auto-revert: per-screen timer returns the sign to AVAILABLE.
// Alert: latches a red overlay, or self-clears after a set time.
//
// Function Select DIP on this board MUST be S1=0 S0=1 for UART1-OUT.
// =====================================================================

LGFX gfx;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf_a;
static lv_color_t *buf_b;

static lv_obj_t *tile_btn[SCREEN_COUNT];
static lv_obj_t *alert_overlay;
static int  current_screen = -1;
static int  avail_index    = 0;

// ---- revert tracking ----
static uint32_t screen_set_ms   = 0;     // millis() when current screen was set
static int      screen_set_mod  = -1;    // minute-of-day when set, -1 if no RTC
static bool     revert_armed    = false;

// ---- alert tracking ----
static bool     alert_active  = false;
static uint32_t alert_start_ms = 0;

// settings page
static lv_obj_t *row_name_lbl;
static lv_obj_t *row_clock_lbl;
static lv_obj_t *row_alert_lbl;

// name editor
static lv_obj_t *name_view;
static lv_obj_t *name_ta;

// clock editor
static lv_obj_t *clock_view;
static lv_obj_t *rol_year, *rol_mon, *rol_day, *rol_hour, *rol_min;

// alert style picker
static lv_obj_t *alert_view;
static lv_obj_t *rol_alert;

// revert timer list + its per-screen picker
static lv_obj_t *revert_view;
static lv_obj_t *revert_list;
static lv_obj_t *revert_row_lbl[SCREEN_COUNT];
static lv_obj_t *revert_val_lbl[SCREEN_COUNT];
static lv_obj_t *rvpick_view;
static lv_obj_t *rol_rvpick;
static lv_obj_t *rvpick_title;
static int       rvpick_idx = -1;

static char    rx_line[64];
static uint8_t rx_len = 0;

// ---------------------------------------------------------------------
// Sakamoto's method. Returns 0=Sunday, matching the BM8563 convention.
static int day_of_week(int y, int m, int d) {
  static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// current minute-of-day, or -1 if the clock has never been set
static int now_mod(void) {
  rtc_time_t t;
  if (!rtc_read(&t)) return -1;
  return rtc_minutes_of_day(&t);
}

// ---------------------------------------------------------------------
static void disp_flush(lv_disp_drv_t *d, const lv_area_t *area, lv_color_t *px) {
  if (gfx.getStartCount() > 0) gfx.endWrite();
  gfx.pushImageDMA(area->x1, area->y1,
                   area->x2 - area->x1 + 1, area->y2 - area->y1 + 1,
                   (lgfx::rgb565_t *)&px->full);
  lv_disp_flush_ready(d);
}

static void touch_read(lv_indev_drv_t *d, lv_indev_data_t *data) {
  uint16_t x, y;
  data->state = LV_INDEV_STATE_REL;
  if (gfx.getTouch(&x, &y)) {
    data->state   = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  }
}

// ---------------------------------------------------------------------
static void send_line(const char *s) {
  Serial1.print(s);
  Serial1.print('\n');
  Serial.printf("-> %s\n", s);
}

static void send_name(void) {
  char msg[8 + CFG_NAME_MAX];
  if (cfg_get_name()[0]) snprintf(msg, sizeof(msg), "NAME %s", cfg_get_name());
  else                   snprintf(msg, sizeof(msg), "NAME");
  send_line(msg);
}

static void select_screen(int idx) {
  if (idx < 0 || idx >= SCREEN_COUNT) return;
  current_screen = idx;

  for (int i = 0; i < SCREEN_COUNT; i++) {
    if (!tile_btn[i]) continue;
    lv_obj_set_style_border_width(tile_btn[i], (i == idx) ? 5 : 0, 0);
    lv_obj_set_style_border_color(tile_btn[i], lv_color_hex(0xFFFFFF), 0);
  }

  // arm the revert timer for this screen
  uint16_t rv = cfg_get_revert(SCREENS[idx].id);
  screen_set_ms  = millis();
  screen_set_mod = now_mod();
  if (rv == RV_NONE) {
    revert_armed = false;
  } else if (rv == RV_EOB || rv == RV_EOD) {
    // clock-based: only arm if the target is still ahead of us today
    int target = (rv == RV_EOB) ? EOB_MOD : EOD_MOD;
    revert_armed = (screen_set_mod >= 0 && screen_set_mod < target);
    if (screen_set_mod >= 0 && !revert_armed)
      Serial.printf("# %s already past, revert not armed\n", revert_label(rv));
    if (screen_set_mod < 0)
      Serial.println("# clock not set, cannot arm EOB/EOD revert");
  } else {
    revert_armed = true;
  }

  char msg[40];
  snprintf(msg, sizeof(msg), "SCR %s", SCREENS[idx].id);
  send_line(msg);

  if (revert_armed)
    Serial.printf("# revert in %s\n", revert_label(rv));
}

static void tile_cb(lv_event_t *e) {
  select_screen((int)(intptr_t)lv_event_get_user_data(e));
}

// ---------------------------------------------------------------------
// alert
// ---------------------------------------------------------------------
static void alert_hide(void) {
  alert_active = false;
  lv_obj_add_flag(alert_overlay, LV_OBJ_FLAG_HIDDEN);
  send_line("ACK");
}

static void alert_show(void) {
  alert_active   = true;
  alert_start_ms = millis();
  lv_obj_clear_flag(alert_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void dismiss_cb(lv_event_t *e) {
  LV_UNUSED(e);
  alert_hide();
}

// ---------------------------------------------------------------------
// settings row labels
// ---------------------------------------------------------------------
static void refresh_name_row(void) {
  char buf[64];
  snprintf(buf, sizeof(buf), "Name            %s",
           cfg_get_name()[0] ? cfg_get_name() : "(none)");
  lv_label_set_text(row_name_lbl, buf);
}

static void refresh_clock_row(void) {
  rtc_time_t t;
  bool ok = rtc_read(&t);
  char buf[72];
  if (ok) {
    snprintf(buf, sizeof(buf), "Clock           %s %02u:%02u  %04u-%02u-%02u",
             RTC_WDAY[t.wday], t.hour, t.minute, t.year, t.month, t.day);
  } else {
    snprintf(buf, sizeof(buf), "Clock           NOT SET");
  }
  lv_label_set_text(row_clock_lbl, buf);
}

static void refresh_alert_row(void) {
  char buf[64];
  snprintf(buf, sizeof(buf), "Alert style     %s", alert_label(cfg_get_alert()));
  lv_label_set_text(row_alert_lbl, buf);
}

static void refresh_revert_row(int idx) {
  if (!revert_row_lbl[idx]) return;

  char label[24];
  strncpy(label, SCREENS[idx].tile, sizeof(label) - 1);
  label[sizeof(label) - 1] = 0;
  for (char *p = label; *p; p++) if (*p == '\n') *p = ' ';

  lv_label_set_text(revert_row_lbl[idx], label);
  lv_label_set_text(revert_val_lbl[idx],
                    revert_label(cfg_get_revert(SCREENS[idx].id)));
}

// ---------------------------------------------------------------------
// generic modal helper
// ---------------------------------------------------------------------
static lv_obj_t *make_modal(void) {
  lv_obj_t *v = lv_obj_create(lv_layer_top());
  lv_obj_set_size(v, LCD_H_RES, LCD_V_RES);
  lv_obj_center(v);
  lv_obj_set_style_bg_color(v, lv_color_hex(0x181D24), 0);
  lv_obj_set_style_radius(v, 0, 0);
  lv_obj_set_style_border_width(v, 0, 0);
  lv_obj_set_style_pad_all(v, 16, 0);
  lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
  return v;
}

static lv_obj_t *make_modal_btn(lv_obj_t *parent, const char *text,
                                lv_coord_t xofs, uint32_t colour,
                                lv_event_cb_t cb) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, 140, 70);
  lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, xofs, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(colour), 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
  lv_obj_center(l);
  return b;
}

// ---------------------------------------------------------------------
// name editor
// ---------------------------------------------------------------------
static void name_save_cb(lv_event_t *e) {
  LV_UNUSED(e);
  cfg_set_name(lv_textarea_get_text(name_ta));
  refresh_name_row();
  send_name();
  lv_obj_add_flag(name_view, LV_OBJ_FLAG_HIDDEN);
}

static void name_cancel_cb(lv_event_t *e) {
  LV_UNUSED(e);
  lv_obj_add_flag(name_view, LV_OBJ_FLAG_HIDDEN);
}

static void name_clear_cb(lv_event_t *e) {
  LV_UNUSED(e);
  lv_textarea_set_text(name_ta, "");
}

static void open_name_view(lv_event_t *e) {
  LV_UNUSED(e);
  lv_textarea_set_text(name_ta, cfg_get_name());
  lv_obj_clear_flag(name_view, LV_OBJ_FLAG_HIDDEN);
}

static void build_name_view(void) {
  name_view = make_modal();
  lv_obj_set_style_pad_all(name_view, 10, 0);

  lv_obj_t *hint = lv_label_create(name_view);
  lv_label_set_text(hint, "Name shown on the wall unit. Leave blank for none.");
  lv_obj_set_style_text_color(hint, lv_color_hex(0xA8B0BA), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 4, 0);

  name_ta = lv_textarea_create(name_view);
  lv_textarea_set_one_line(name_ta, true);
  lv_textarea_set_max_length(name_ta, CFG_NAME_MAX - 1);
  lv_obj_set_size(name_ta, 460, 56);
  lv_obj_align(name_ta, LV_ALIGN_TOP_LEFT, 4, 30);
  lv_obj_set_style_text_font(name_ta, &lv_font_montserrat_28, 0);

  lv_obj_t *bc = lv_btn_create(name_view);
  lv_obj_set_size(bc, 100, 56);
  lv_obj_align(bc, LV_ALIGN_TOP_LEFT, 476, 30);
  lv_obj_set_style_bg_color(bc, lv_color_hex(0x4A5462), 0);
  lv_obj_add_event_cb(bc, name_clear_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bcl = lv_label_create(bc);
  lv_label_set_text(bcl, "Clear");
  lv_obj_center(bcl);

  lv_obj_t *bx = lv_btn_create(name_view);
  lv_obj_set_size(bx, 100, 56);
  lv_obj_align(bx, LV_ALIGN_TOP_LEFT, 584, 30);
  lv_obj_set_style_bg_color(bx, lv_color_hex(0x8A3B3B), 0);
  lv_obj_add_event_cb(bx, name_cancel_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bxl = lv_label_create(bx);
  lv_label_set_text(bxl, "Cancel");
  lv_obj_center(bxl);

  lv_obj_t *bs = lv_btn_create(name_view);
  lv_obj_set_size(bs, 100, 56);
  lv_obj_align(bs, LV_ALIGN_TOP_LEFT, 692, 30);
  lv_obj_set_style_bg_color(bs, lv_color_hex(0x1E7A3C), 0);
  lv_obj_add_event_cb(bs, name_save_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bsl = lv_label_create(bs);
  lv_label_set_text(bsl, "Save");
  lv_obj_center(bsl);

  lv_obj_t *kb = lv_keyboard_create(name_view);
  lv_obj_set_size(kb, LCD_H_RES - 20, 290);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(kb, name_ta);
}

// ---------------------------------------------------------------------
// clock editor
// ---------------------------------------------------------------------
static void clock_save_cb(lv_event_t *e) {
  LV_UNUSED(e);
  rtc_time_t t;
  t.year   = 2026 + lv_roller_get_selected(rol_year);
  t.month  = 1 + lv_roller_get_selected(rol_mon);
  t.day    = 1 + lv_roller_get_selected(rol_day);
  t.hour   = lv_roller_get_selected(rol_hour);
  t.minute = lv_roller_get_selected(rol_min);
  t.second = 0;
  t.wday   = day_of_week(t.year, t.month, t.day);

  Serial.printf("# set clock %04u-%02u-%02u %s %02u:%02u\n",
                t.year, t.month, t.day, RTC_WDAY[t.wday], t.hour, t.minute);
  Serial.println(rtc_write(&t) ? "# rtc write ok" : "# RTC WRITE FAILED");

  refresh_clock_row();
  lv_obj_add_flag(clock_view, LV_OBJ_FLAG_HIDDEN);
}

static void clock_cancel_cb(lv_event_t *e) {
  LV_UNUSED(e);
  lv_obj_add_flag(clock_view, LV_OBJ_FLAG_HIDDEN);
}

static void open_clock_view(lv_event_t *e) {
  LV_UNUSED(e);
  rtc_time_t t;
  if (rtc_read(&t) && t.year >= 2026 && t.year <= 2035) {
    lv_roller_set_selected(rol_year, t.year - 2026, LV_ANIM_OFF);
    lv_roller_set_selected(rol_mon,  t.month - 1,   LV_ANIM_OFF);
    lv_roller_set_selected(rol_day,  t.day - 1,     LV_ANIM_OFF);
    lv_roller_set_selected(rol_hour, t.hour,        LV_ANIM_OFF);
    lv_roller_set_selected(rol_min,  t.minute,      LV_ANIM_OFF);
  }
  lv_obj_clear_flag(clock_view, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_roller(lv_obj_t *parent, const char *opts,
                             lv_coord_t x, lv_coord_t w, const char *cap) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, cap);
  lv_obj_set_style_text_color(l, lv_color_hex(0xA8B0BA), 0);
  lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, 60);

  lv_obj_t *r = lv_roller_create(parent);
  lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(r, 4);
  lv_obj_set_width(r, w);
  lv_obj_align(r, LV_ALIGN_TOP_LEFT, x, 90);
  lv_obj_set_style_text_font(r, &lv_font_montserrat_28, 0);
  return r;
}

static void build_clock_view(void) {
  static char opt_year[80], opt_mon[64], opt_day[160],
              opt_hour[160], opt_min[400];
  char *p;

  p = opt_year; for (int i = 2026; i <= 2035; i++) p += sprintf(p, i == 2035 ? "%d" : "%d\n", i);
  p = opt_mon;  for (int i = 1;    i <= 12;   i++) p += sprintf(p, i == 12   ? "%02d" : "%02d\n", i);
  p = opt_day;  for (int i = 1;    i <= 31;   i++) p += sprintf(p, i == 31   ? "%02d" : "%02d\n", i);
  p = opt_hour; for (int i = 0;    i <= 23;   i++) p += sprintf(p, i == 23   ? "%02d" : "%02d\n", i);
  p = opt_min;  for (int i = 0;    i <= 59;   i++) p += sprintf(p, i == 59   ? "%02d" : "%02d\n", i);

  clock_view = make_modal();

  lv_obj_t *hint = lv_label_create(clock_view);
  lv_label_set_text(hint, "Set clock. Weekday is calculated from the date.");
  lv_obj_set_style_text_color(hint, lv_color_hex(0xA8B0BA), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 0);

  rol_year = make_roller(clock_view, opt_year,   0, 150, "Year");
  rol_mon  = make_roller(clock_view, opt_mon,  165, 100, "Month");
  rol_day  = make_roller(clock_view, opt_day,  280, 100, "Day");
  rol_hour = make_roller(clock_view, opt_hour, 430, 100, "Hour");
  rol_min  = make_roller(clock_view, opt_min,  545, 100, "Minute");

  make_modal_btn(clock_view, "Cancel", 300, 0x8A3B3B, clock_cancel_cb);
  make_modal_btn(clock_view, "Save",   460, 0x1E7A3C, clock_save_cb);
}

// ---------------------------------------------------------------------
// alert style picker
// ---------------------------------------------------------------------
static void alert_save_cb(lv_event_t *e) {
  LV_UNUSED(e);
  cfg_set_alert((uint8_t)lv_roller_get_selected(rol_alert));
  refresh_alert_row();
  lv_obj_add_flag(alert_view, LV_OBJ_FLAG_HIDDEN);
}

static void alert_cancel_cb(lv_event_t *e) {
  LV_UNUSED(e);
  lv_obj_add_flag(alert_view, LV_OBJ_FLAG_HIDDEN);
}

static void open_alert_view(lv_event_t *e) {
  LV_UNUSED(e);
  lv_roller_set_selected(rol_alert, cfg_get_alert(), LV_ANIM_OFF);
  lv_obj_clear_flag(alert_view, LV_OBJ_FLAG_HIDDEN);
}

static void build_alert_view(void) {
  alert_view = make_modal();

  lv_obj_t *hint = lv_label_create(alert_view);
  lv_label_set_text(hint, "How long the desk alert stays up before clearing itself.");
  lv_obj_set_style_text_color(hint, lv_color_hex(0xA8B0BA), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 0);

  rol_alert = lv_roller_create(alert_view);
  lv_roller_set_options(rol_alert, ALERT_OPTIONS, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(rol_alert, 4);
  lv_obj_set_width(rol_alert, 500);
  lv_obj_align(rol_alert, LV_ALIGN_TOP_MID, 0, 60);
  lv_obj_set_style_text_font(rol_alert, &lv_font_montserrat_28, 0);

  make_modal_btn(alert_view, "Cancel", 300, 0x8A3B3B, alert_cancel_cb);
  make_modal_btn(alert_view, "Save",   460, 0x1E7A3C, alert_save_cb);
}

// ---------------------------------------------------------------------
// per-screen revert picker
// ---------------------------------------------------------------------
static void rvpick_save_cb(lv_event_t *e) {
  LV_UNUSED(e);
  if (rvpick_idx >= 0) {
    uint16_t v = REVERT_VALUES[lv_roller_get_selected(rol_rvpick)];
    cfg_set_revert(SCREENS[rvpick_idx].id, v);
    refresh_revert_row(rvpick_idx);
    if (rvpick_idx == current_screen) select_screen(rvpick_idx);  // re-arm
  }
  lv_obj_add_flag(rvpick_view, LV_OBJ_FLAG_HIDDEN);
}

static void rvpick_cancel_cb(lv_event_t *e) {
  LV_UNUSED(e);
  lv_obj_add_flag(rvpick_view, LV_OBJ_FLAG_HIDDEN);
}

static void open_rvpick(lv_event_t *e) {
  rvpick_idx = (int)(intptr_t)lv_event_get_user_data(e);
  lv_label_set_text(rvpick_title, "Revert to AVAILABLE after");
  lv_roller_set_selected(rol_rvpick,
                         revert_opt_index(cfg_get_revert(SCREENS[rvpick_idx].id)),
                         LV_ANIM_OFF);
  lv_obj_clear_flag(rvpick_view, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(rvpick_view);   // must sit above the revert list
}

static void build_rvpick_view(void) {
  rvpick_view = make_modal();

  rvpick_title = lv_label_create(rvpick_view);
  lv_label_set_text(rvpick_title, "");
  lv_obj_set_style_text_color(rvpick_title, lv_color_hex(0xA8B0BA), 0);
  lv_obj_align(rvpick_title, LV_ALIGN_TOP_LEFT, 0, 0);

  rol_rvpick = lv_roller_create(rvpick_view);
  lv_roller_set_options(rol_rvpick, REVERT_OPTIONS, LV_ROLLER_MODE_NORMAL);
  lv_roller_set_visible_row_count(rol_rvpick, 4);
  lv_obj_set_width(rol_rvpick, 560);
  lv_obj_align(rol_rvpick, LV_ALIGN_TOP_MID, 0, 60);
  lv_obj_set_style_text_font(rol_rvpick, &lv_font_montserrat_28, 0);

  make_modal_btn(rvpick_view, "Cancel", 300, 0x8A3B3B, rvpick_cancel_cb);
  make_modal_btn(rvpick_view, "Save",   460, 0x1E7A3C, rvpick_save_cb);
}

// ---------------------------------------------------------------------
// revert timer list
// ---------------------------------------------------------------------
static void revert_close_cb(lv_event_t *e) {
  LV_UNUSED(e);
  lv_obj_add_flag(revert_view, LV_OBJ_FLAG_HIDDEN);
}

static void open_revert_view(lv_event_t *e) {
  LV_UNUSED(e);
  for (int i = 0; i < SCREEN_COUNT; i++) refresh_revert_row(i);
  lv_obj_clear_flag(revert_view, LV_OBJ_FLAG_HIDDEN);
}

static void build_revert_view(void) {
  revert_view = make_modal();

  lv_obj_t *hint = lv_label_create(revert_view);
  lv_label_set_text(hint, "Auto-revert each status to AVAILABLE. Tap to change.");
  lv_obj_set_style_text_color(hint, lv_color_hex(0xA8B0BA), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 0);

  revert_list = lv_obj_create(revert_view);
  lv_obj_set_size(revert_list, LCD_H_RES - 40, 320);
  lv_obj_align(revert_list, LV_ALIGN_TOP_MID, 0, 32);
  lv_obj_set_style_bg_opa(revert_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(revert_list, 0, 0);
  lv_obj_set_style_pad_all(revert_list, 4, 0);
  lv_obj_set_flex_flow(revert_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(revert_list, 6, 0);

  for (int i = 0; i < SCREEN_COUNT; i++) {
    lv_obj_t *b = lv_btn_create(revert_list);
    lv_obj_set_size(b, LCD_H_RES - 76, 54);
    lv_obj_set_style_bg_color(b, lv_color_hex(SCREENS[i].bg), 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_add_event_cb(b, open_rvpick, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    // status name, left column
    revert_row_lbl[i] = lv_label_create(b);
    lv_obj_set_style_text_font(revert_row_lbl[i], &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(revert_row_lbl[i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(revert_row_lbl[i], LV_ALIGN_LEFT_MID, 12, 0);

    // timer value, fixed column so every row lines up
    revert_val_lbl[i] = lv_label_create(b);
    lv_obj_set_style_text_font(revert_val_lbl[i], &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(revert_val_lbl[i], lv_color_hex(0xEAEAEA), 0);
    lv_obj_align(revert_val_lbl[i], LV_ALIGN_LEFT_MID, 360, 0);

    refresh_revert_row(i);
  }

  make_modal_btn(revert_view, "Close", 330, 0x4A5462, revert_close_cb);
}

// ---------------------------------------------------------------------
// settings page
// ---------------------------------------------------------------------
static lv_obj_t *add_setting_row(lv_obj_t *parent, const char *text,
                                 lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, LCD_H_RES - 60, 62);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x232A33), 0);
  lv_obj_set_style_radius(btn, 8, 0);
  if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *l = lv_label_create(btn);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
  return l;
}

static void build_settings_page(lv_obj_t *tile) {
  lv_obj_set_style_pad_all(tile, 20, 0);
  lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(tile, 10, 0);

  lv_obj_t *title = lv_label_create(tile);
  lv_label_set_text(title, "SETTINGS");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x8A94A0), 0);

  row_name_lbl  = add_setting_row(tile, "Name",  open_name_view);
  row_clock_lbl = add_setting_row(tile, "Clock", open_clock_view);
  row_alert_lbl = add_setting_row(tile, "Alert style", open_alert_view);
                  add_setting_row(tile, "Revert timers", open_revert_view);

  refresh_name_row();
  refresh_clock_row();
  refresh_alert_row();
}

// ---------------------------------------------------------------------
static void build_alert_overlay(void) {
  alert_overlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(alert_overlay, LCD_H_RES, LCD_V_RES);
  lv_obj_center(alert_overlay);
  lv_obj_set_style_bg_color(alert_overlay, lv_color_hex(0xC0161B), 0);
  lv_obj_set_style_bg_opa(alert_overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(alert_overlay, 0, 0);
  lv_obj_set_style_radius(alert_overlay, 0, 0);
  lv_obj_clear_flag(alert_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(alert_overlay, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *l = lv_label_create(alert_overlay);
  lv_label_set_text(l, "SOMEONE AT YOUR DESK");
  lv_obj_set_style_text_font(l, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, -80);

  lv_obj_t *btn = lv_btn_create(alert_overlay);
  lv_obj_set_size(btn, 320, 100);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 70);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_add_event_cb(btn, dismiss_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *bl = lv_label_create(btn);
  lv_label_set_text(bl, "DISMISS");
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(bl, lv_color_hex(0xC0161B), 0);
  lv_obj_center(bl);
}

static void build_ui(void) {
  static lv_coord_t col_dsc[GRID_COLS + 1];
  static lv_coord_t row_dsc[GRID_ROWS + 1];
  for (int i = 0; i < GRID_COLS; i++) col_dsc[i] = LV_GRID_FR(1);
  col_dsc[GRID_COLS] = LV_GRID_TEMPLATE_LAST;
  for (int i = 0; i < GRID_ROWS; i++) row_dsc[i] = LV_GRID_FR(1);
  row_dsc[GRID_ROWS] = LV_GRID_TEMPLATE_LAST;

  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x101418), 0);

  lv_obj_t *tv = lv_tileview_create(lv_scr_act());
  lv_obj_set_size(tv, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);

  for (int page = 0; page < PAGE_COUNT; page++) {
    lv_obj_t *tile = lv_tileview_add_tile(tv, page, 0, LV_DIR_HOR);
    lv_obj_set_style_pad_all(tile, 14, 0);
    lv_obj_set_grid_dsc_array(tile, col_dsc, row_dsc);
    lv_obj_set_style_pad_row(tile, 14, 0);
    lv_obj_set_style_pad_column(tile, 14, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    for (int slot = 0; slot < TILES_PER_PAGE; slot++) {
      int idx = page * TILES_PER_PAGE + slot;
      if (idx >= SCREEN_COUNT) break;

      lv_obj_t *btn = lv_btn_create(tile);
      lv_obj_set_grid_cell(btn,
                           LV_GRID_ALIGN_STRETCH, slot % GRID_COLS, 1,
                           LV_GRID_ALIGN_STRETCH, slot / GRID_COLS, 1);
      lv_obj_set_style_bg_color(btn, lv_color_hex(SCREENS[idx].bg), 0);
      lv_obj_set_style_radius(btn, 12, 0);
      lv_obj_set_style_border_width(btn, 0, 0);
      lv_obj_add_event_cb(btn, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

      lv_obj_t *lbl = lv_label_create(btn);
      lv_label_set_text(lbl, SCREENS[idx].tile);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
      lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(lbl);

      tile_btn[idx] = btn;
    }
  }

  // settings lives on the page after the last grid page
  lv_obj_t *set_tile = lv_tileview_add_tile(tv, PAGE_COUNT, 0, LV_DIR_HOR);
  build_settings_page(set_tile);

  build_alert_overlay();
  build_name_view();
  build_clock_view();
  build_alert_view();
  build_rvpick_view();
  build_revert_view();
}

// ---------------------------------------------------------------------
static void handle_line(const char *line) {
  Serial.printf("<- %s\n", line);

  if (strcmp(line, "ALERT") == 0) {
    alert_show();
  } else if (strcmp(line, "HELLO") == 0) {
    send_name();
    if (current_screen >= 0) {
      char msg[40];
      snprintf(msg, sizeof(msg), "SCR %s", SCREENS[current_screen].id);
      send_line(msg);
    }
  }
}

static void poll_link(void) {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') {
      rx_line[rx_len] = 0;
      if (rx_len) handle_line(rx_line);
      rx_len = 0;
    } else if (c != '\r' && rx_len < sizeof(rx_line) - 1) {
      rx_line[rx_len++] = c;
    }
  }
}

// ---------------------------------------------------------------------
// once a second: revert timer, alert timeout, settings clock row
// ---------------------------------------------------------------------
static void tick_cb(lv_timer_t *t) {
  LV_UNUSED(t);

  // ---- alert auto-clear ----
  if (alert_active) {
    uint32_t limit = cfg_alert_ms();
    if (limit && (millis() - alert_start_ms) >= limit) {
      Serial.println("# alert auto-cleared");
      alert_hide();
    }
  }

  // ---- auto-revert ----
  if (revert_armed && current_screen >= 0) {
    uint16_t rv = cfg_get_revert(SCREENS[current_screen].id);
    bool fire = false;

    if (rv == RV_EOB || rv == RV_EOD) {
      int target = (rv == RV_EOB) ? EOB_MOD : EOD_MOD;
      int mod = now_mod();
      if (mod >= 0 && screen_set_mod >= 0 &&
          mod >= target && screen_set_mod < target) fire = true;
    } else if (rv != RV_NONE) {
      if ((millis() - screen_set_ms) >= (uint32_t)rv * 60000UL) fire = true;
    }

    if (fire) {
      Serial.printf("# auto-revert from %s\n", SCREENS[current_screen].id);
      revert_armed = false;
      select_screen(avail_index);
    }
  }

  refresh_clock_row();
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n--- control_panel ---");

  Serial1.begin(LINK_BAUD, SERIAL_8N1, CTRL_UART_RX, CTRL_UART_TX);

  panel_hw_init();
  Serial.printf("# panel %s\n", panel_hw_name());
  Serial.printf("# RTC %s\n", rtc_present() ? "present" : "NOT FOUND");

  cfg_begin();
  Serial.printf("# name '%s', alert %s\n",
                cfg_get_name(), alert_label(cfg_get_alert()));

  avail_index = screen_index_by_id("avail");
  if (avail_index < 0) {
    avail_index = 0;
    Serial.println("# WARNING: no screen with id 'avail', reverting to index 0");
  }

  gfx.init();
  gfx.initDMA();
  gfx.startWrite();
  gfx.fillScreen(TFT_BLACK);

  // splash, drawn direct to the panel before LVGL takes over the display
  if (FFat.begin(false, "/ffat", 2) && FFat.exists("/splash.png")) {
    gfx.drawPngFile(FFat, "/splash.png", 0, 0);
    delay(2000);
  }

  lv_init();
  const size_t lines = 80;
  const size_t bytes = sizeof(lv_color_t) * LCD_H_RES * lines;
  buf_a = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  buf_b = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&draw_buf, buf_a, buf_b, LCD_H_RES * lines);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = LCD_H_RES;
  disp_drv.ver_res  = LCD_V_RES;
  disp_drv.flush_cb = disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  build_ui();
  select_screen(avail_index);
  send_name();

  lv_timer_create(tick_cb, 1000, NULL);

  Serial.printf("%d screens, %d grid pages + settings\n",
                SCREEN_COUNT, PAGE_COUNT);
}

void loop() {
  lv_timer_handler();
  poll_link();
  delay(1);
}

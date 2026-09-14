#include <Arduino.h>
#include <FFat.h>
#include "LovyanGFX_Driver.h"
#include "panel_hw.h"
#include "sign_config.h"

// =====================================================================
// display_panel — wall unit, slave. No LVGL.
// Full-screen image per status, drawn straight from the FatFS partition.
// Receives SCR <id> and ACK on UART0-IN (IO43/IO44 = Serial).
// Tap anywhere: sends ALERT, shows confirmation until the desk ACKs.
// =====================================================================

LGFX gfx;

static int     current_screen = -1;
static bool    alert_pending  = false;
static bool    fs_ok          = false;
static bool    was_touched    = false;
static char    rx_line[64];
static uint8_t rx_len = 0;
static char dp_name[24] = "";

#define TOAST_W  620
#define TOAST_H  110
#define TOAST_X  ((LCD_H_RES - TOAST_W) / 2)
#define TOAST_Y  (LCD_V_RES - TOAST_H - 20)
#define BAND_W   700
#define BAND_H   80
#define BAND_X   ((LCD_H_RES - BAND_W) / 2)
#define BAND_Y   (LCD_V_RES - BAND_H - 16)

// ---------------------------------------------------------------------
// standing hint — visible whenever there is no pending alert
static void draw_hint(void) {
  char msg[80];
  build_hint_msg(msg, sizeof(msg), dp_name);

  gfx.fillRoundRect(BAND_X, BAND_Y, BAND_W, BAND_H, 14, gfx.color888(16, 20, 26));
  gfx.setTextColor(gfx.color888(200, 208, 216));
  gfx.setFont(&fonts::FreeSans12pt7b);
  gfx.setTextDatum(middle_center);
  gfx.drawString(msg, LCD_H_RES / 2, BAND_Y + BAND_H / 2);
  gfx.setTextDatum(top_left);
}

// confirmation — replaces the hint until the desk acknowledges
static void draw_toast(void) {
  char msg[64];
  build_notify_msg(msg, sizeof(msg), dp_name);

  gfx.fillRoundRect(BAND_X, BAND_Y, BAND_W, BAND_H, 14, gfx.color888(12, 105, 48));
  gfx.setTextColor(TFT_WHITE);
  gfx.setFont(&fonts::FreeSansBold18pt7b);
  gfx.setTextDatum(middle_center);
  gfx.drawString(msg, LCD_H_RES / 2, BAND_Y + BAND_H / 2);
  gfx.setTextDatum(top_left);
}

static void draw_screen(int idx) {
  if (idx < 0 || idx >= SCREEN_COUNT) return;
  const screen_def_t *s = &SCREENS[idx];

  gfx.fillScreen(gfx.color888((s->bg >> 16) & 0xFF,
                              (s->bg >> 8)  & 0xFF,
                               s->bg        & 0xFF));

  bool drawn = false;
  if (fs_ok && s->img) {
    uint32_t t0 = millis();
    drawn = gfx.drawPngFile(FFat, s->img, 0, 0);
    Serial.printf("# %s %s %s in %lums\n",
                  s->id, s->img, drawn ? "ok" : "FAILED", millis() - t0);
  }

  if (!drawn) {
    gfx.setTextColor(TFT_WHITE);
    gfx.setTextDatum(middle_center);
    gfx.setFont(&fonts::FreeSansBold24pt7b);
    gfx.drawString(s->headline, LCD_H_RES / 2, LCD_V_RES / 2 - 30);
    gfx.setFont(&fonts::FreeSans12pt7b);
    if (s->sub) gfx.drawString(s->sub, LCD_H_RES / 2, LCD_V_RES / 2 + 40);
    gfx.setTextDatum(top_left);
  }

   if (alert_pending) draw_toast();
  else               draw_hint();
}

static void apply_screen(int idx) {
  if (idx < 0 || idx >= SCREEN_COUNT) return;
  current_screen = idx;
  draw_screen(idx);
}

// ---------------------------------------------------------------------
static void alert_show() {
  if (alert_pending) return;
  alert_pending = true;
  draw_toast();
}

static void alert_clear(void) {
  if (!alert_pending) return;
  alert_pending = false;
  draw_hint();                       // band reverts to the hint, no full repaint
}

static void poll_touch() {
  uint16_t x, y;
  bool now = gfx.getTouch(&x, &y);
  if (now && !was_touched) {
    Serial.println("ALERT");
    alert_show();
  }
  was_touched = now;
}

// ---------------------------------------------------------------------
static void handle_line(const char *line) {
  if (strncmp(line, "SCR ", 4) == 0) {
    int idx = screen_index_by_id(line + 4);
    if (idx >= 0) apply_screen(idx);
    else Serial.printf("# unknown screen id '%s'\n", line + 4);

  } else if (strcmp(line, "ACK") == 0) {
    alert_clear();

  } else if (strncmp(line, "NAME ", 5) == 0) {
    strncpy(dp_name, line + 5, sizeof(dp_name) - 1);
    dp_name[sizeof(dp_name) - 1] = 0;
    if (alert_pending) draw_toast(); else draw_hint();

  } else if (strcmp(line, "NAME") == 0) {
    dp_name[0] = 0;
    if (alert_pending) draw_toast(); else draw_hint();
  }
}

static void poll_link() {
  while (Serial.available()) {
    char c = Serial.read();
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
void setup() {
  Serial.begin(LINK_BAUD);          // UART0-IN, down the cable

  panel_hw_init();
  Serial.printf("# panel %s\n", panel_hw_name());

  fs_ok = FFat.begin(false, "/ffat", 2);
  Serial.printf("# FFat %s\n", fs_ok ? "mounted" : "MOUNT FAILED");

  gfx.init();
  gfx.initDMA();
  gfx.setColorDepth(16);
  gfx.fillScreen(TFT_BLACK);

   if (fs_ok && FFat.exists("/splash.png")) {
    gfx.drawPngFile(FFat, "/splash.png", 0, 0);
    delay(2000);
  }
  apply_screen(0);

  delay(300);
  Serial.println("HELLO");
}

void loop() {
  poll_touch();
  poll_link();
  delay(10);
}

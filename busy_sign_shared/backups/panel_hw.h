#ifndef PANEL_HW_H
#define PANEL_HW_H

#include <Arduino.h>
#include <Wire.h>

// =====================================================================
// CrowPanel Advance 4.3" hardware abstraction.
// Detects board revision at boot and hides the backlight difference:
//   V1.0   TCA9534 @ 0x18, P1 = backlight enable. ON/OFF ONLY.
//   V1.1+  STC8H1K28 @ 0x30, value 0-245 INVERTED (0 = brightest).
// Call panel_hw_init() once, then panel_backlight(0..255) freely.
// =====================================================================

#define I2C_SDA          15
#define I2C_SCL          16
#define GT911_INT_PIN    1

#define TCA_ADDR         0x18
#define STC8_ADDR        0x30
#define RTC_ADDR         0x51
#define GT911_ADDR       0x5D

#define TCA_REG_OUTPUT   0x01
#define TCA_REG_CONFIG   0x03
#define TCA_CFG_OUTPUTS  0xE1     // P1..P4 outputs, rest inputs
#define TCA_BIT_BL       0x02     // P1 = LCD backlight enable
#define TCA_BIT_TPRST    0x04     // P2 = GT911 reset

#define STC8_CMD_TOUCH   250      // activate touch controller

typedef enum { PANEL_HW_UNKNOWN = 0, PANEL_HW_V10, PANEL_HW_V13 } panel_hw_t;

static panel_hw_t panel_hw   = PANEL_HW_UNKNOWN;
static uint8_t    tca_shadow = 0;
static uint8_t    bl_level   = 255;

static inline bool i2c_present(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static inline bool tca_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static inline bool stc8_cmd(uint8_t v) {
  Wire.beginTransmission(STC8_ADDR);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

// 0 = off, 255 = brightest. V1.0 has no dimming: any non-zero is full on.
static inline void panel_backlight(uint8_t level) {
  bl_level = level;
  if (panel_hw == PANEL_HW_V13) {
    uint16_t v = 245 - ((uint16_t)level * 245 / 255);
    stc8_cmd((uint8_t)v);
  } else {
    if (level) tca_shadow |=  TCA_BIT_BL;
    else       tca_shadow &= ~TCA_BIT_BL;
    tca_write(TCA_REG_OUTPUT, tca_shadow);
  }
}

static inline uint8_t panel_backlight_level(void) { return bl_level; }

static inline bool panel_has_dimming(void) { return panel_hw == PANEL_HW_V13; }

static inline const char *panel_hw_name(void) {
  switch (panel_hw) {
    case PANEL_HW_V10: return "V1.0 (TCA9534, on/off only)";
    case PANEL_HW_V13: return "V1.1+ (STC8 0x30, dimmable)";
    default:           return "UNKNOWN";
  }
}

// I2C up, revision detected, backlight on, GT911 reset so it answers at 0x5D.
static inline void panel_hw_init(void) {
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);

  if      (i2c_present(STC8_ADDR)) panel_hw = PANEL_HW_V13;
  else if (i2c_present(TCA_ADDR))  panel_hw = PANEL_HW_V10;
  else                             panel_hw = PANEL_HW_UNKNOWN;

  if (panel_hw == PANEL_HW_V13) {
    for (int i = 0; i < 20; i++) {
      if (i2c_present(STC8_ADDR) && i2c_present(GT911_ADDR)) break;
      stc8_cmd(STC8_CMD_TOUCH);
      pinMode(GT911_INT_PIN, OUTPUT);
      digitalWrite(GT911_INT_PIN, LOW);
      delay(120);
      pinMode(GT911_INT_PIN, INPUT);
      delay(100);
    }
    panel_backlight(255);
  } else {
    tca_write(TCA_REG_CONFIG, TCA_CFG_OUTPUTS);
    tca_shadow = TCA_BIT_BL;                    // backlight on, TP reset asserted
    tca_write(TCA_REG_OUTPUT, tca_shadow);
    pinMode(GT911_INT_PIN, OUTPUT);
    digitalWrite(GT911_INT_PIN, LOW);           // selects GT911 address 0x5D
    delay(20);
    tca_shadow |= TCA_BIT_TPRST;                // release TP reset
    tca_write(TCA_REG_OUTPUT, tca_shadow);
    delay(100);
    pinMode(GT911_INT_PIN, INPUT);
  }
}

#endif

#ifndef RTC_BM8563_H
#define RTC_BM8563_H

#include <Arduino.h>
#include <Wire.h>

// =====================================================================
// BM8563 / PCF8563 real-time clock, I2C 0x51.
// Confirmed from the V1.0 schematic: BM8563EMA + 32.768kHz + CR1220.
// Assumes Wire.begin() has already run (panel_hw_init does it).
// =====================================================================

#define RTC_I2C_ADDR    0x51
#define RTC_REG_CTRL1   0x00
#define RTC_REG_CTRL2   0x01
#define RTC_REG_SECONDS 0x02   // bit7 = VL: 1 means the clock is unreliable

typedef struct {
  uint16_t year;    // full year, e.g. 2026
  uint8_t  month;   // 1-12
  uint8_t  day;     // 1-31
  uint8_t  wday;    // 0=Sun .. 6=Sat
  uint8_t  hour;    // 0-23
  uint8_t  minute;
  uint8_t  second;
} rtc_time_t;

static inline uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static inline uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static inline bool rtc_present(void) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  return Wire.endTransmission() == 0;
}

static inline bool rtc_read(rtc_time_t *t) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(RTC_REG_SECONDS);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)RTC_I2C_ADDR, (uint8_t)7) != 7) return false;

  uint8_t s  = Wire.read();
  uint8_t mi = Wire.read();
  uint8_t h  = Wire.read();
  uint8_t d  = Wire.read();
  uint8_t wd = Wire.read();
  uint8_t mo = Wire.read();
  uint8_t y  = Wire.read();

  bool vl = (s & 0x80) != 0;          // oscillator stopped since last set

  t->second = bcd2dec(s  & 0x7F);
  t->minute = bcd2dec(mi & 0x7F);
  t->hour   = bcd2dec(h  & 0x3F);
  t->day    = bcd2dec(d  & 0x3F);
  t->wday   = wd & 0x07;
  t->month  = bcd2dec(mo & 0x1F);
  t->year   = 2000 + bcd2dec(y) + ((mo & 0x80) ? 100 : 0);

  return !vl;                          // false = time is not trustworthy
}

static inline bool rtc_write(const rtc_time_t *t) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(RTC_REG_SECONDS);
  Wire.write(dec2bcd(t->second) & 0x7F);      // clears VL
  Wire.write(dec2bcd(t->minute) & 0x7F);
  Wire.write(dec2bcd(t->hour)   & 0x3F);
  Wire.write(dec2bcd(t->day)    & 0x3F);
  Wire.write(t->wday & 0x07);
  Wire.write((dec2bcd(t->month) & 0x1F) | ((t->year >= 2100) ? 0x80 : 0x00));
  Wire.write(dec2bcd((uint8_t)(t->year % 100)));
  return Wire.endTransmission() == 0;
}

// minutes since midnight — the unit schedules and revert timers work in
static inline int rtc_minutes_of_day(const rtc_time_t *t) {
  return t->hour * 60 + t->minute;
}

static inline bool rtc_is_weekend(const rtc_time_t *t) {
  return t->wday == 0 || t->wday == 6;
}

static const char *RTC_WDAY[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };

#endif
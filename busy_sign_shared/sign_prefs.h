#ifndef SIGN_PREFS_H
#define SIGN_PREFS_H

#include <Arduino.h>
#include <Preferences.h>

// =====================================================================
// Persistent settings, NVS namespace "sign".
// Per-screen values key off the permanent screen id, so reordering or
// adding screens can never corrupt them. NVS caps key names at 15 chars;
// ids are <= 7, so "rv_" + id fits.
// =====================================================================

#define CFG_NAME_MAX  24

// revert sentinels — real values are minutes
#define RV_NONE   0
#define RV_EOB    1001    // 17:00
#define RV_EOD    1002    // 23:59
#define EOB_MOD   (17 * 60)
#define EOD_MOD   (23 * 60 + 59)

// alert style
#define AL_LATCH  0
#define AL_5S     1
#define AL_1M     2
#define AL_5M     3

static Preferences cfg_prefs;
static char        cfg_name[CFG_NAME_MAX] = "";
static uint8_t     cfg_alert = AL_LATCH;

static inline void cfg_begin(void) {
  cfg_prefs.begin("sign", false);
  cfg_prefs.getString("name", cfg_name, sizeof(cfg_name));
  cfg_alert = cfg_prefs.getUChar("alert", AL_LATCH);
}

static inline const char *cfg_get_name(void) { return cfg_name; }

static inline void cfg_set_name(const char *s) {
  strncpy(cfg_name, s ? s : "", sizeof(cfg_name) - 1);
  cfg_name[sizeof(cfg_name) - 1] = 0;
  cfg_prefs.putString("name", cfg_name);
}

static inline uint8_t cfg_get_alert(void) { return cfg_alert; }

static inline void cfg_set_alert(uint8_t v) {
  cfg_alert = v;
  cfg_prefs.putUChar("alert", v);
}

// milliseconds before the alert self-clears, 0 = latch forever
static inline uint32_t cfg_alert_ms(void) {
  switch (cfg_alert) {
    case AL_5S: return 5000;
    case AL_1M: return 60000;
    case AL_5M: return 300000;
    default:    return 0;
  }
}

static inline uint16_t cfg_get_revert(const char *id) {
  char key[16];
  snprintf(key, sizeof(key), "rv_%s", id);
  return cfg_prefs.getUShort(key, RV_NONE);
}

static inline void cfg_set_revert(const char *id, uint16_t v) {
  char key[16];
  snprintf(key, sizeof(key), "rv_%s", id);
  cfg_prefs.putUShort(key, v);
}

static inline const char *revert_label(uint16_t v) {
  switch (v) {
    case RV_NONE: return "None";
    case 10:      return "10 min";
    case 15:      return "15 min";
    case 30:      return "30 min";
    case 60:      return "1 hour";
    case 120:     return "2 hours";
    case RV_EOB:  return "End of business";
    case RV_EOD:  return "End of day";
    default:      return "?";
  }
}

static const uint16_t REVERT_VALUES[] = {
  RV_NONE, 10, 15, 30, 60, 120, RV_EOB, RV_EOD
};
#define REVERT_OPT_COUNT ((int)(sizeof(REVERT_VALUES)/sizeof(REVERT_VALUES[0])))

#define REVERT_OPTIONS \
  "None\n10 min\n15 min\n30 min\n1 hour\n2 hours\n" \
  "End of business (5pm)\nEnd of day (11:59pm)"

#define ALERT_OPTIONS \
  "Latch until dismissed\n5 seconds\n1 minute\n5 minutes"

static inline const char *alert_label(uint8_t v) {
  switch (v) {
    case AL_5S: return "5 seconds";
    case AL_1M: return "1 minute";
    case AL_5M: return "5 minutes";
    default:    return "Latch";
  }
}

static inline int revert_opt_index(uint16_t v) {
  for (int i = 0; i < REVERT_OPT_COUNT; i++)
    if (REVERT_VALUES[i] == v) return i;
  return 0;
}

#endif
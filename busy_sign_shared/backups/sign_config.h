#ifndef SIGN_CONFIG_H
#define SIGN_CONFIG_H

#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------
// Shared config for control_panel and display_panel.
// Hardlink this file between both sketch folders — a stale copy causes
// silent wrong-screen bugs with no compile error.
//
// To add a screen: append a row to SCREENS[]. Nothing else.
//   - "id" is the permanent wire identifier. NEVER change or reuse one.
//   - Row order sets grid position and is safe to change.
//   - Pages are generated automatically.
// ---------------------------------------------------------------------

#define LINK_BAUD       115200

// control_panel transmits on UART1: IO19=RX, IO20=TX.
// Its Function Select DIP MUST be S1=0 S0=1.
#define CTRL_UART_RX    19
#define CTRL_UART_TX    20

// display_panel receives on UART0-IN (IO43/IO44) = default Serial.

#define LCD_H_RES       800
#define LCD_V_RES       480

#define GRID_COLS       3
#define GRID_ROWS       2
#define TILES_PER_PAGE  (GRID_COLS * GRID_ROWS)



// ---- wire protocol, newline terminated ASCII ----
//   control -> display :  SCR <id>    select screen by permanent id
//   control -> display :  ACK         desk acknowledged the alert
//   display -> control :  ALERT       someone tapped the wall unit
//   display -> control :  HELLO       just booted, resend current state

typedef struct {
  const char *id;        // permanent wire id — never change once shipped
  const char *tile;      // label on the control panel tile
  const char *headline;  // fallback text if the image is missing
  const char *sub;       // fallback second line
  uint32_t    bg;        // display background, and the tile colour
  const char *img;       // filesystem path, or NULL for text only
} screen_def_t;

static const screen_def_t SCREENS[] = {
  { "avail",   "AVAILABLE",          "AVAILABLE",         "Come on in",            0x0C6930, "/available.png" },
  { "busy",    "BUSY",               "BUSY",              "Interrupt if urgent",   0xAA5702, "/busy.png"      },
  { "dnd",     "DO NOT\nDISTURB",    "DO NOT DISTURB",    "Deep work in progress", 0x9A1315, "/dnd.png"       },
  { "meeting", "IN A\nMEETING",      "IN A MEETING",      "Back shortly",          0x114692, "/meeting.png"   },
  { "call",    "ON A CALL",          "ON A CALL",         "Please don't knock",    0x532E88, "/phone.png"     },
  { "soon",    "BACK SOON",          "BACK SOON",         "Stepped away",          0x0D7473, "/backsoon.png"  },
  { "lunch",   "AT LUNCH",           "AT LUNCH",          "Back after lunch",      0xBA4D0D, "/lunch.png"     },
  { "floor",   "WALKING\nTHE FLOOR", "WALKING THE FLOOR", "Working elsewhere",     0x0143AC, "/walking.png"   },
  { "ooo",     "OUT OF\nOFFICE",     "OUT OF OFFICE",     "Not in the building",   0x404958, "/ooo.png"       },
  { "eod",     "GONE FOR\nTHE DAY",  "GONE FOR THE DAY",  "See you tomorrow",      0x454E5B, "/home.png"      },
};

#define SCREEN_COUNT ((int)(sizeof(SCREENS) / sizeof(SCREENS[0])))
#define PAGE_COUNT   ((SCREEN_COUNT + TILES_PER_PAGE - 1) / TILES_PER_PAGE)

static inline int screen_index_by_id(const char *id) {
  if (!id) return -1;
  for (int i = 0; i < SCREEN_COUNT; i++) {
    if (strcmp(SCREENS[i].id, id) == 0) return i;
  }
  return -1;
}

// occupant name, "" for none
#define OCCUPANT_NAME   "Jess"

static inline const char *notify_msg(void) {
  static char buf[64];
  if (OCCUPANT_NAME[0]) {
    snprintf(buf, sizeof(buf), "%s has been notified", OCCUPANT_NAME);
  } else {
    snprintf(buf, sizeof(buf), "Notification has been sent.");
  }
  return buf;
}

#endif

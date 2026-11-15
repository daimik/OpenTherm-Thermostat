// Event log of the display.
// elog() prints a line on the serial port and keeps it in a ring of the last EVLOG_LINES lines for the
// status page. It may be called from any task (main loop, Bluetooth, MQTT, Wi-Fi events), never from an
// interrupt. The ring lives in PSRAM; if that is not available the line is only printed.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "state.h"

#define EVLOG_LINES 300
#define EVLOG_TEXT  112

struct EvLine {
  uint32_t seq;               // 1, 2, 3 ... since boot
  uint32_t ms;                // millis() when it was written
  char     text[EVLOG_TEXT];
};

void     evlog_init();                              // first thing in setup()
void     elog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t evlog_newest();                            // sequence number of the newest line, 0 = nothing yet
uint32_t evlog_oldest();                            // oldest line still in the ring
bool     evlog_get(uint32_t seq, EvLine *out);      // false when that line is gone or not written yet

// Main task, once a second: writes a line for every part of the state that changed since the last call
// (mode, target, heating, preheat, sensor, boiler link, flame, faults, MQTT ...).
void     evlog_state_tick(const ThermoState &s);

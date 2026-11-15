// Event log of the boiler node.
// nlog() prints a line on the serial port and, when the web interface is built in, also keeps it in a
// ring of the last NLOG_LINES lines for the status page. Call it from tasks only, never from an interrupt.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define NLOG_LINES 200
#define NLOG_TEXT  104

struct LogLine {
  uint32_t seq;               // 1, 2, 3 ... since boot
  uint32_t ms;                // millis() when it was written
  char     text[NLOG_TEXT];
};

void     nlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t nlog_newest();                          // sequence number of the newest line, 0 = nothing logged yet
uint32_t nlog_oldest();                          // oldest line still in the ring
bool     nlog_get(uint32_t seq, LogLine *out);   // false when that line is gone or not written yet

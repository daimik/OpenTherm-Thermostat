#include "nodelog.h"
#include "node_web.h"
#include <Arduino.h>
#include <stdarg.h>
#include <string.h>

#if NODE_WEB
static LogLine ring[NLOG_LINES];
static uint32_t newest = 0;                                   // line n lives in ring[n % NLOG_LINES]
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;       // held only for one small copy
#endif

void nlog(const char *fmt, ...) {
  char buf[NLOG_TEXT];
  va_list a;
  va_start(a, fmt);
  vsnprintf(buf, sizeof(buf), fmt, a);
  va_end(a);
  Serial.println(buf);
#if NODE_WEB
  uint32_t ms = millis();
  portENTER_CRITICAL(&mux);
  LogLine &l = ring[(newest + 1) % NLOG_LINES];
  l.seq = ++newest;
  l.ms = ms;
  memcpy(l.text, buf, sizeof(l.text));
  portEXIT_CRITICAL(&mux);
#endif
}

#if NODE_WEB
uint32_t nlog_newest() { return newest; }
uint32_t nlog_oldest() {
  uint32_t n = newest;
  return n == 0 ? 0 : (n > NLOG_LINES ? n - NLOG_LINES + 1 : 1);
}
bool nlog_get(uint32_t seq, LogLine *out) {
  bool ok = false;
  portENTER_CRITICAL(&mux);
  const LogLine &l = ring[seq % NLOG_LINES];
  if (seq != 0 && l.seq == seq) { *out = l; ok = true; }
  portEXIT_CRITICAL(&mux);
  return ok;
}
#else
uint32_t nlog_newest() { return 0; }
uint32_t nlog_oldest() { return 0; }
bool nlog_get(uint32_t, LogLine *) { return false; }
#endif

// Read-only status page of the display. See web.h for what it is and is not.
//
// Requests (all GET, nothing else is answered):
//   /              the page
//   /api/status    everything the page shows, as JSON
//   /api/log       event log as JSON, ?since=<sequence> returns only newer lines
//   /api/history   24 hours of samples for the charts, one a minute
//   /log.txt       event log as plain text
//
// Uses the WebServer library that ships with the ESP32 Arduino core (checked against the 2.0.17 header
// this project builds with): handleClient() returns at once when nobody is connected, a response of
// unknown length is sent with setContentLength(CONTENT_LENGTH_UNKNOWN) and sendContent(), and an empty
// sendContent("") ends it.

#include "web.h"
#include "secrets.h"

#ifdef WEB_PASS
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <stdarg.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "state.h"
#include "schedule.h"
#include "thermo_logic.h"
#include "evlog.h"
#include "web_page.h"

#ifndef WEB_USER
#define WEB_USER "admin"
#endif

#define SAMPLE_MS   60000UL          // one chart sample a minute
#define SAMPLES     1440             // 24 hours
#define NO_VALUE    INT16_MIN        // "not known at that moment": the chart leaves a gap

struct Sample {
  int16_t room, target, req, flow, ret, outside;   // 0.1 degC
  uint8_t mod;
  uint8_t fl;                        // bit 0 heating asked, bit 1 flame, bit 2 preheat
};

// ---- handed over by the main task, guarded by vmux ----
static SemaphoreHandle_t vmux = nullptr;
static ThermoState    view;
static ThermoSchedule viewSched;
static Sample  *hist = nullptr;      // ring, in PSRAM
static uint16_t histCount = 0, histHead = 0;
static uint32_t lastSampleMs = 0;

// ---- web task only ----
static WebServer http(80);
static ThermoState    cur;           // the copy a request works on
static ThermoSchedule curSched;
static Sample  *hcopy = nullptr;
static char     out[3072];
static uint32_t bootId = 0;

static int16_t c10(float v) { return (int16_t)lroundf(v * 10); }

void web_publish() {
  // Never wait: if a request is copying the data right now, this second is simply skipped.
  if (!vmux || xSemaphoreTake(vmux, 0) != pdTRUE) return;
  view = st;
  viewSched = sched;
  uint32_t now = millis();
  if (hist && (now - lastSampleMs >= SAMPLE_MS || histCount == 0)) {
    lastSampleMs = now;
    bool boiler = st.bleLinked && st.boilerLinked;
    Sample &s = hist[histHead];
    s.room    = st.sensorOk ? c10(st.room) : NO_VALUE;
    s.target  = st.mode == MODE_OFF ? NO_VALUE : c10(st.target);
    s.req     = st.heating ? c10(st.flowRequest) : NO_VALUE;
    s.flow    = boiler ? c10(st.flow) : NO_VALUE;
    s.ret     = boiler ? c10(st.ret) : NO_VALUE;
    s.outside = st.outsideOk ? c10(st.outside) : NO_VALUE;
    s.mod     = (boiler && st.flame) ? st.modulation : 0;
    s.fl      = (st.heating ? 1 : 0) | ((boiler && st.flame) ? 2 : 0) | (st.preheat ? 4 : 0);
    histHead = (histHead + 1) % SAMPLES;
    if (histCount < SAMPLES) histCount++;
  }
  xSemaphoreGive(vmux);
}

// ---------- small text helpers ----------
struct Buf {
  char  *p;
  size_t cap, len;
};
static void bprintf(Buf &b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bprintf(Buf &b, const char *fmt, ...) {
  if (b.len + 1 >= b.cap) return;
  va_list a;
  va_start(a, fmt);
  int n = vsnprintf(b.p + b.len, b.cap - b.len, fmt, a);
  va_end(a);
  if (n < 0) return;
  b.len += (size_t)n < b.cap - b.len ? (size_t)n : b.cap - b.len - 1;
}
// A JSON string, quotes included. Anything that could break the JSON is escaped.
static void bjson(Buf &b, const char *s) {
  bprintf(b, "\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') bprintf(b, "\\%c", c);
    else if (c < 0x20)         bprintf(b, "\\u%04x", c);
    else                       bprintf(b, "%c", c);
  }
  bprintf(b, "\"");
}
// "key":12.3, or "key":null, when the value is not known
static void bnum(Buf &b, const char *key, bool known, float v, int decimals) {
  if (known) bprintf(b, "\"%s\":%.*f,", key, decimals, (double)v);
  else       bprintf(b, "\"%s\":null,", key);
}
static const char *tf(bool x) { return x ? "true" : "false"; }

static time_t clock_now() {                       // 0 until NTP has set the system clock
  time_t t = time(nullptr);
  return t > 1700000000 ? t : 0;
}

// ---------- access ----------
// HTTP Basic login when a password is configured. Wrong guesses are slowed to one per second;
// that only delays this page, never the thermostat.
static bool allowed() {
  if (sizeof(WEB_PASS) <= 1) return true;
  if (http.authenticate(WEB_USER, WEB_PASS)) return true;
  static uint32_t fails = 0, lastFailMs = 0;
  uint32_t now = millis();
  if (now - lastFailMs > 10000) fails = 0;
  lastFailMs = now;
  if (++fails > 5) delay(1000);
  http.requestAuthentication(BASIC_AUTH, "CV Thermostat", "Login required");
  return false;
}
static void common_headers() {
  http.sendHeader("Cache-Control", "no-store");
  http.sendHeader("X-Content-Type-Options", "nosniff");
}
static void take_view() {
  xSemaphoreTake(vmux, portMAX_DELAY);
  cur = view;
  curSched = viewSched;
  xSemaphoreGive(vmux);
}

// ---------- handlers ----------
static void h_root() {
  if (!allowed()) return;
  common_headers();
  http.sendHeader("Content-Security-Policy", "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'");
  http.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
}

static void h_status() {
  if (!allowed()) return;
  take_view();
  const ThermoState &s = cur;
  const CtrlParams lim = {};                       // the control loop runs with these defaults (control.cpp)
  bool boiler = s.bleLinked && s.boilerLinked;
  uint32_t now = millis();
  Buf b = { out, sizeof(out), 0 };
  static const char *const kDays[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
  static const char *const kModes[3] = { "auto", "manual", "off" };

  bprintf(b, "{\"boot\":%lu,\"up\":%lu,\"upms\":%lu,\"epoch\":%lld,", (unsigned long)bootId,
          (unsigned long)(now / 1000), (unsigned long)now, (long long)clock_now());
  if (s.clockOk) bprintf(b, "\"clock\":\"%s %02u:%02u\",", kDays[s.dow % 7], s.hour, s.minute);
  else           bprintf(b, "\"clock\":\"\",");

  bprintf(b, "\"room\":{");
  bnum(b, "t", s.sensorOk, s.room, 1);
  bnum(b, "h", s.sensorOk, s.humidity, 0);
  bnum(b, "raw", s.sensorPresent, s.roomRaw, 2);
  bprintf(b, "\"ok\":%s,\"present\":%s,\"fault\":%s,\"offset\":%.1f,\"rejects\":%lu},", tf(s.sensorOk), tf(s.sensorPresent),
          tf(s.sensorFault), (double)s.tempOffset, (unsigned long)s.sensorRejects);

  bprintf(b, "\"set\":{\"mode\":\"%s\",\"target\":%.1f,\"heating\":%s,\"preheat\":%s,\"frost\":%s,\"auto\":%s,\"override\":%s,\"next\":%u,\"rate\":%.2f},",
          kModes[s.mode <= MODE_OFF ? s.mode : MODE_OFF], (double)s.target, tf(s.heating), tf(s.preheat), tf(s.frostActive),
          tf(s.autoActive), tf(s.overrideActive), (unsigned)s.nextChangeMin, (double)s.heatRate);

  float err = s.target - s.room;
  bprintf(b, "\"ctl\":{\"demand\":%s,\"err\":%.2f,\"base\":%.1f,\"p\":%.2f,\"i\":%.2f,\"req\":%.1f,", tf(s.ctrlDemand), (double)err,
          (double)s.ctrlBase, (double)(lim.kp * err), (double)s.ctrlInteg, (double)s.flowRequest);
  bnum(b, "outside", s.outsideOk, s.outside, 1);
  bprintf(b, "\"outage\":%lu},", (unsigned long)(s.outsideOk ? (now - s.outsideMs) / 1000 : 0));

  bprintf(b, "\"boiler\":{");
  bnum(b, "flow", boiler, s.flow, 1);
  bnum(b, "ret", boiler, s.ret, 1);
  bnum(b, "dhw", boiler && s.dhw != 0.0f, s.dhw, 1);
  bnum(b, "pressure", boiler && s.pressure != 0.0f, s.pressure, 2);
  bprintf(b, "\"ok\":%s,\"sim\":%s,\"flame\":%s,\"mod\":%u,\"burn\":%lu,\"gas\":%.2f,\"fault\":%s,\"code\":", tf(boiler), tf(s.boilerSimulated),
          tf(boiler && s.flame), (unsigned)((boiler && s.flame) ? s.modulation : 0), (unsigned long)s.burnerSecondsToday, (double)s.gasToday,
          tf(s.boilerFault));
  bjson(b, s.lastFault);
  bprintf(b, ",\"fallback\":%s,\"nfrost\":%s,\"clamped\":%s,\"hold\":%s},", tf(s.bleLinked && s.boilerFallback), tf(s.nodeFrost), tf(s.nodeClamped), tf(s.nodeHold));

  bprintf(b, "\"ble\":{\"linked\":%s,\"rssi\":%d,\"reconn\":%lu,\"pinned\":%s,\"authfail\":%s,\"mismatch\":%s},", tf(s.bleLinked), s.bleRssi,
          (unsigned long)s.bleReconnects, tf(s.nodePinned), tf(s.bleAuthFailed), tf(s.bleVersionMismatch));

  bprintf(b, "\"net\":{\"ssid\":");
  bjson(b, WIFI_SSID);
  bprintf(b, ",\"bssid\":");
  bjson(b, WiFi.BSSIDstr().c_str());
  bprintf(b, ",\"ch\":%d,\"rssi\":%d,\"ip\":", (int)WiFi.channel(), (int)WiFi.RSSI());
  bjson(b, s.ip);
  bprintf(b, ",\"mqtt\":%s},", tf(s.haOk));

  bprintf(b, "\"sys\":{\"reset\":");
  bjson(b, s.resetReason);
  bprintf(b, ",\"crashes\":%u,\"heap\":%lu,\"heapmin\":%lu,\"psram\":%lu,"
             "\"slow\":%lu,\"slowmax\":%u,\"bright\":%u,\"sleep\":%s,\"sounds\":%s,\"vol\":%u},",
          (unsigned)s.crashCount,
          (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
          (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned long)s.slowFrames, (unsigned)s.slowFrameWorstMs,
          (unsigned)s.brightness, tf(s.sleepScreen), tf(s.sounds), (unsigned)s.volume);

  bprintf(b, "\"sched\":{");
  for (int k = 0; k < 2; k++) {
    const SchedBlock *d = k ? curSched.weekend : curSched.weekday;
    bprintf(b, "%s\"%s\":[", k ? "," : "", k ? "we" : "wd");
    for (int i = 0; i < SCHED_BLOCKS; i++) bprintf(b, "%s[%u,%d]", i ? "," : "", (unsigned)d[i].startMin, (int)d[i].temp_c10);
    bprintf(b, "]");
  }
  bprintf(b, "},");

  bprintf(b, "\"lim\":{\"hon\":%.2f,\"hoff\":%.2f,\"fmin\":%.0f,\"fmax\":%.0f}}", (double)lim.hystOn, (double)lim.hystOff, (double)lim.flowMin, (double)lim.flowMax);

  common_headers();
  http.send(200, "application/json", out);
}

// Sends what is in the buffer as one chunk and empties it.
static void flush(Buf &b) {
  if (b.len) http.sendContent(b.p, b.len);
  b.len = 0;
  b.p[0] = 0;
}

static void h_log() {
  if (!allowed()) return;
  uint32_t since = http.hasArg("since") ? strtoul(http.arg("since").c_str(), nullptr, 10) : 0;
  uint32_t newest = evlog_newest(), oldest = evlog_oldest();
  if (since > newest) since = 0;                          // the page is older than this boot: start over
  uint32_t from = since + 1 > oldest ? since + 1 : oldest;

  common_headers();
  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, "application/json", "");
  Buf b = { out, sizeof(out), 0 };
  bprintf(b, "{\"now\":%lu,\"epoch\":%lld,\"lines\":[", (unsigned long)millis(), (long long)clock_now());
  bool first = true;
  static EvLine l;
  for (uint32_t seq = from; seq != 0 && seq <= newest; seq++) {
    if (!evlog_get(seq, &l)) continue;                    // overwritten while we were sending
    if (b.len > sizeof(out) - 6 * EVLOG_TEXT - 40) flush(b);   // worst case: every character escaped
    bprintf(b, "%s[%lu,%lu,", first ? "" : ",", (unsigned long)l.seq, (unsigned long)l.ms);
    bjson(b, l.text);
    bprintf(b, "]");
    first = false;
  }
  bprintf(b, "]}");
  flush(b);
  http.sendContent("");
}

static void h_logtxt() {
  if (!allowed()) return;
  uint32_t newest = evlog_newest();
  time_t nowT = clock_now();
  uint32_t nowMs = millis();

  common_headers();
  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, "text/plain; charset=utf-8", "");
  Buf b = { out, sizeof(out), 0 };
  bprintf(b, "CV Thermostat display, up %lu s\r\n", (unsigned long)(nowMs / 1000));
  if (!nowT) bprintf(b, "clock not set yet: times are seconds since boot\r\n");
  bprintf(b, "\r\n");
  static EvLine l;
  for (uint32_t seq = evlog_oldest(); seq != 0 && seq <= newest; seq++) {
    if (!evlog_get(seq, &l)) continue;
    if (b.len > sizeof(out) - EVLOG_TEXT - 60) flush(b);
    if (nowT) {
      time_t t = nowT - (time_t)((uint32_t)(nowMs - l.ms) / 1000);
      struct tm tmv;
      localtime_r(&t, &tmv);
      char ts[24];
      strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
      bprintf(b, "%s  %s\r\n", ts, l.text);
    } else {
      bprintf(b, "+%8lu.%03lu  %s\r\n", (unsigned long)(l.ms / 1000), (unsigned long)(l.ms % 1000), l.text);
    }
  }
  flush(b);
  http.sendContent("");
}

static void h_history() {
  if (!allowed()) return;
  uint16_t n = 0, head = 0;
  if (hist && hcopy) {
    xSemaphoreTake(vmux, portMAX_DELAY);
    memcpy(hcopy, hist, sizeof(Sample) * SAMPLES);
    n = histCount;
    head = histHead;
    xSemaphoreGive(vmux);
  }
  uint16_t start = (uint16_t)((head + SAMPLES - n) % SAMPLES);   // oldest sample

  common_headers();
  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, "application/json", "");
  Buf b = { out, sizeof(out), 0 };
  bprintf(b, "{\"step\":%lu", (unsigned long)(SAMPLE_MS / 1000));
  static const char *const kNames[8] = { "room", "target", "req", "flow", "ret", "out", "mod", "fl" };
  for (int k = 0; k < 8; k++) {
    bprintf(b, ",\"%s\":[", kNames[k]);
    for (uint16_t i = 0; i < n; i++) {
      const Sample &s = hcopy[(start + i) % SAMPLES];
      int val = k == 0 ? s.room : k == 1 ? s.target : k == 2 ? s.req : k == 3 ? s.flow : k == 4 ? s.ret : k == 5 ? s.outside : k == 6 ? s.mod : s.fl;
      if (b.len > sizeof(out) - 16) flush(b);
      bprintf(b, "%s%d", i ? "," : "", val);
    }
    bprintf(b, "]");
  }
  bprintf(b, "}");
  flush(b);
  http.sendContent("");
}

static void h_notfound() {
  if (!allowed()) return;
  common_headers();
  http.send(404, "text/plain", "Not found. This device only answers /, /api/status, /api/log, /api/history and /log.txt\r\n");
}

// ---------- task ----------
static void web_task(void *) {
  http.on("/", HTTP_GET, h_root);
  http.on("/api/status", HTTP_GET, h_status);
  http.on("/api/log", HTTP_GET, h_log);
  http.on("/api/history", HTTP_GET, h_history);
  http.on("/log.txt", HTTP_GET, h_logtxt);
  http.onNotFound(h_notfound);

  bool serving = false;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }   // no Wi-Fi: nothing to do
    if (!serving) {
      http.begin();
      serving = true;
      elog("status page at http://%s/", WiFi.localIP().toString().c_str());
    }
    http.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void web_init() {
  if (vmux) return;
  vmux = xSemaphoreCreateMutex();
  // The chart samples (two copies of 20 kB) live in PSRAM. Without them the page works, minus the charts.
  hist  = (Sample *)heap_caps_calloc(SAMPLES, sizeof(Sample), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  hcopy = (Sample *)heap_caps_calloc(SAMPLES, sizeof(Sample), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  bootId = esp_random() | 1;
  // Core 0, beside the MQTT task; the display and the control loop run on core 1.
  xTaskCreatePinnedToCore(web_task, "web", 8192, NULL, 1, NULL, 0);
}

#else   // no WEB_PASS in secrets.h: no page, no server

void web_init() {}
void web_publish() {}

#endif

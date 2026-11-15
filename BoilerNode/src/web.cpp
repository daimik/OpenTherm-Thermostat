// Read-only status page of the boiler node. See include/node_web.h for what it is and is not.
//
// Requests (all GET, nothing else is answered):
//   /              the page
//   /api/status    everything the page shows, as JSON
//   /api/log       event log as JSON, ?since=<sequence> returns only newer lines
//   /api/history   one hour of samples for the chart
//   /log.txt       event log as plain text
//
// Written against the WebServer and WiFi documentation of the ESP32 Arduino core 3.x:
//   - handleClient() returns at once when nobody is connected and yields while it waits, so it can
//     live in its own task
//   - a response of unknown length is sent with setContentLength(CONTENT_LENGTH_UNKNOWN) and
//     sendContent(); an empty sendContent("") ends it
//   - Wi-Fi modem sleep stays on (WiFi.setSleep(true)): Wi-Fi and Bluetooth share the one radio

#include "node_web.h"

#if NODE_WEB
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <stdarg.h>
#include <time.h>
#include "esp_random.h"
#include "esp_system.h"
#include "node_secrets.h"
#include "nodelog.h"
#include "web_page.h"

// The two Wi-Fi lines may be copied unchanged from the display's secrets.h: both spellings are accepted.
#if !defined(NODE_WIFI_SSID) && defined(WIFI_SSID)
#define NODE_WIFI_SSID WIFI_SSID
#endif
#if !defined(NODE_WIFI_PASS) && defined(WIFI_PASS)
#define NODE_WIFI_PASS WIFI_PASS
#endif
#if !defined(NODE_WIFI_SSID) || !defined(NODE_WIFI_PASS)
#error "node_secrets.h must define the Wi-Fi name and password: NODE_WIFI_SSID and NODE_WIFI_PASS (or WIFI_SSID and WIFI_PASS)"
#endif
#ifndef NODE_HOSTNAME
#define NODE_HOSTNAME "cv-boiler"
#endif
#ifndef NODE_WEB_USER
#define NODE_WEB_USER "admin"
#endif
#ifndef NODE_WEB_PASS
#define NODE_WEB_PASS ""
#endif
#ifndef NODE_TZ
#define NODE_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

#define SAMPLE_MS   10000UL          // one chart sample every 10 s
#define SAMPLES     360              // one hour

struct Sample {
  int16_t flow, ret, sp;             // 0.1 degC
  uint8_t mod;
  uint8_t fl;                        // bit 0 flame, bit 1 heating enabled, bit 2 hot water, bit 3 boiler link
};

// ---- handed over by the main loop ----
static portMUX_TYPE vmux = portMUX_INITIALIZER_UNLOCKED;
static NodeView view;                // protected by vmux
static Sample   hist[SAMPLES];       // protected by vmux, ring
static uint16_t histCount = 0, histHead = 0;
static uint32_t lastSampleMs = 0;
static volatile uint8_t bleStateNow = 0;

// ---- fixed at start ----
static const char *buildName = "";
static int resetReason = 0;
static SafetyParams limits;
static uint32_t bootId = 0;

// ---- web task only ----
static WebServer http(80);
static NodeView  v;                  // the copy a request works on
static Sample    hcopy[SAMPLES];
static char      out[2048];

void web_publish(const NodeView &nv) {
  uint32_t now = millis();
  portENTER_CRITICAL(&vmux);
  view = nv;
  if (now - lastSampleMs >= SAMPLE_MS || histCount == 0) {
    lastSampleMs = now;
    Sample &s = hist[histHead];
    s.flow = nv.st.flow_c10;
    s.ret = nv.st.return_c10;
    s.sp = nv.chOn ? nv.st.ctrl_setpoint_c10 : 0;         // 0 = no heating asked: the chart leaves a gap
    s.mod = (nv.st.flags & BF_FLAME) ? nv.st.modulation : 0;
    s.fl = ((nv.st.flags & BF_FLAME) ? 1 : 0) | (nv.chOn ? 2 : 0) | ((nv.st.flags & BF_DHW_ACTIVE) ? 4 : 0) | ((nv.st.flags & BF_OT_LINK) ? 8 : 0);
    histHead = (histHead + 1) % SAMPLES;
    if (histCount < SAMPLES) histCount++;
  }
  portEXIT_CRITICAL(&vmux);
  bleStateNow = nv.bleState;
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
// A temperature in 0.1 degC as JSON: null when it was never read.
static void btemp(Buf &b, const char *key, int16_t c10, bool known) {
  if (known) bprintf(b, "\"%s\":%.1f,", key, c10 / 10.0);
  else       bprintf(b, "\"%s\":null,", key);
}
static const char *tf(bool x) { return x ? "true" : "false"; }

static const char *reset_text(int r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_EXT:      return "reset pin";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "crash (panic)";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog (main loop hung)";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_BROWNOUT: return "brownout (supply voltage dipped)";
    default:               return r == 0 ? "unknown" : "other";
  }
}

static time_t clock_now() {                       // 0 until NTP has set the clock
  time_t t = time(nullptr);
  return t > 1700000000 ? t : 0;
}

// ---------- access ----------
// HTTP Basic login when a password is configured. Wrong guesses are slowed to one per second;
// that only delays this page, never the heating.
static bool allowed() {
  if (sizeof(NODE_WEB_PASS) <= 1) return true;
  if (http.authenticate(NODE_WEB_USER, NODE_WEB_PASS)) return true;
  static uint32_t fails = 0, lastFailMs = 0;
  uint32_t now = millis();
  if (now - lastFailMs > 10000) fails = 0;
  lastFailMs = now;
  if (++fails > 5) delay(1000);
  http.requestAuthentication(BASIC_AUTH, "CV-Boiler node", "Login required");
  return false;
}
static void common_headers() {
  http.sendHeader("Cache-Control", "no-store");
  http.sendHeader("X-Content-Type-Options", "nosniff");
}
static void take_view() {
  portENTER_CRITICAL(&vmux);
  v = view;
  portEXIT_CRITICAL(&vmux);
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
  const BoilerStatus &s = v.st;
  bool link = (s.flags & BF_OT_LINK) != 0;
  Buf b = { out, sizeof(out), 0 };

  bprintf(b, "{\"boot\":%lu,\"build\":", (unsigned long)bootId);
  bjson(b, buildName);
  bprintf(b, ",\"up\":%lu,\"upms\":%lu,\"epoch\":%lld,\"reset\":", (unsigned long)(millis() / 1000), (unsigned long)millis(), (long long)clock_now());
  bjson(b, reset_text(resetReason));
  bprintf(b, ",\"heap\":%lu,\"heapmin\":%lu,", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());

  bprintf(b, "\"wifi\":{\"ssid\":");
  bjson(b, NODE_WIFI_SSID);
  bprintf(b, ",\"bssid\":");
  bjson(b, WiFi.BSSIDstr().c_str());
  bprintf(b, ",\"ch\":%d,\"rssi\":%d,\"ip\":", (int)WiFi.channel(), (int)WiFi.RSSI());
  bjson(b, WiFi.localIP().toString().c_str());
  bprintf(b, "},");

  bprintf(b, "\"boiler\":{\"link\":%s,\"sim\":%s,\"flame\":%s,\"ch\":%s,\"dhw\":%s,\"fault\":%s,\"diag\":%s,",
          tf(link), tf(v.simulated), tf(s.flags & BF_FLAME), tf(s.flags & BF_CH_ACTIVE), tf(s.flags & BF_DHW_ACTIVE),
          tf(s.flags & BF_FAULT), tf(s.flags & BF_DIAG));
  btemp(b, "flow", s.flow_c10, s.flow_c10 != 0);
  btemp(b, "ret", s.return_c10, s.return_c10 != 0);
  btemp(b, "dhwt", s.dhw_c10, s.dhw_c10 != 0);
  btemp(b, "outside", s.outside_c10, s.outside_c10 != INT16_MIN);
  if (s.pressure_mbar) bprintf(b, "\"pressure\":%.2f,", s.pressure_mbar / 1000.0);
  else                 bprintf(b, "\"pressure\":null,");
  bprintf(b, "\"mod\":%u,\"faultflags\":%u,\"oemfault\":%u,\"oemdiag\":%u,\"starts\":%lu,\"burn\":%lu},",
          (unsigned)((s.flags & BF_FLAME) ? s.modulation : 0), (unsigned)s.fault_flags, (unsigned)s.oem_fault, (unsigned)s.oem_diag,
          (unsigned long)v.flameStarts, (unsigned long)v.flameOnS);

  bprintf(b, "\"ot\":{\"last\":");
  bjson(b, v.otLast);
  bprintf(b, ",\"ok\":%lu,\"timeout\":%lu,\"invalid\":%lu,\"readerr\":%lu,\"edges\":%lu,\"rx\":%u,\"pinrx\":%u,\"pintx\":%u,\"missing\":%u},",
          (unsigned long)v.otOk, (unsigned long)v.otTimeout, (unsigned long)v.otInvalid, (unsigned long)v.otReadErr,
          (unsigned long)v.otEdges, (unsigned)v.otRxLevel, (unsigned)v.pinRx, (unsigned)v.pinTx, (unsigned)v.otMissing);

  bprintf(b, "\"ctl\":{\"have\":%s,\"age\":%.1f,\"flow\":%.1f,\"ch\":%s,\"dhw\":%s,\"dhwsp\":%.1f,\"frames\":%lu},",
          tf(v.haveControl), v.controlAgeMs / 1000.0, v.req.ch_setpoint_c10 / 10.0, tf(v.req.flags & CF_CH_ENABLE),
          tf(v.req.flags & CF_DHW_ENABLE), v.req.dhw_setpoint_c10 / 10.0, (unsigned long)v.ctrlFrames);

  bprintf(b, "\"out\":{\"sp\":%.1f,\"ch\":%s,\"fallback\":%s,\"frost\":%s,\"clamped\":%s,\"hold\":%s},",
          s.ctrl_setpoint_c10 / 10.0, tf(v.chOn), tf(v.fallback), tf(v.frost), tf(v.clamped), tf(v.hold));

  static const char *const kBle[3] = { "advertising", "connected, not authenticated", "session" };
  char peer[20] = "";
  if (v.havePeer) snprintf(peer, sizeof(peer), "%02x:%02x:%02x:%02x:%02x:%02x", v.peer[5], v.peer[4], v.peer[3], v.peer[2], v.peer[1], v.peer[0]);
  bprintf(b, "\"ble\":{\"state\":\"%s\",\"peer\":\"%s\",\"sessions\":%lu,\"drops\":%lu,\"reason\":%d,\"rejected\":%lu,\"streak\":%lu},",
          kBle[v.bleState < 3 ? v.bleState : 0], peer, (unsigned long)v.sessions, (unsigned long)v.drops, v.lastDropReason,
          (unsigned long)v.rejected, (unsigned long)v.unauthStreak);

  bprintf(b, "\"lim\":{\"min\":%.0f,\"max\":%.0f,\"fb\":%.0f,\"fbafter\":%lu,\"minon\":%lu,\"minoff\":%lu,\"froston\":%.0f,\"frostoff\":%.0f}}",
          limits.flowMin_c10 / 10.0, limits.flowMax_c10 / 10.0, limits.fallbackFlow_c10 / 10.0, (unsigned long)(limits.fallbackMs / 1000),
          (unsigned long)(limits.minOnMs / 1000), (unsigned long)(limits.minOffMs / 1000), limits.frostOn_c10 / 10.0, limits.frostOff_c10 / 10.0);

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
  uint32_t newest = nlog_newest(), oldest = nlog_oldest();
  if (since > newest) since = 0;                          // the page is older than this boot: start over
  uint32_t from = since + 1 > oldest ? since + 1 : oldest;

  common_headers();
  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, "application/json", "");
  Buf b = { out, sizeof(out), 0 };
  bprintf(b, "{\"now\":%lu,\"epoch\":%lld,\"lines\":[", (unsigned long)millis(), (long long)clock_now());
  bool first = true;
  LogLine l;
  for (uint32_t seq = from; seq != 0 && seq <= newest; seq++) {
    if (!nlog_get(seq, &l)) continue;                     // overwritten while we were sending
    if (b.len > sizeof(out) - 6 * NLOG_TEXT - 40) flush(b);   // worst case: every character escaped
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
  uint32_t newest = nlog_newest();
  time_t nowT = clock_now();
  uint32_t nowMs = millis();

  common_headers();
  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, "text/plain; charset=utf-8", "");
  Buf b = { out, sizeof(out), 0 };
  bprintf(b, "CV-Boiler node (%s), up %lu s, last reset: %s\r\n", buildName, (unsigned long)(nowMs / 1000), reset_text(resetReason));
  if (!nowT) bprintf(b, "clock not set yet: times are seconds since boot\r\n");
  bprintf(b, "\r\n");
  LogLine l;
  for (uint32_t seq = nlog_oldest(); seq != 0 && seq <= newest; seq++) {
    if (!nlog_get(seq, &l)) continue;
    if (b.len > sizeof(out) - NLOG_TEXT - 60) flush(b);
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
  uint16_t n, head;
  portENTER_CRITICAL(&vmux);
  memcpy(hcopy, hist, sizeof(hist));
  n = histCount;
  head = histHead;
  portEXIT_CRITICAL(&vmux);
  uint16_t start = (uint16_t)((head + SAMPLES - n) % SAMPLES);   // oldest sample

  common_headers();
  http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  http.send(200, "application/json", "");
  Buf b = { out, sizeof(out), 0 };
  bprintf(b, "{\"step\":%lu", (unsigned long)(SAMPLE_MS / 1000));
  static const char *const kNames[5] = { "flow", "ret", "sp", "mod", "fl" };
  for (int k = 0; k < 5; k++) {
    bprintf(b, ",\"%s\":[", kNames[k]);
    for (uint16_t i = 0; i < n; i++) {
      const Sample &s = hcopy[(start + i) % SAMPLES];
      int val = k == 0 ? s.flow : k == 1 ? s.ret : k == 2 ? s.sp : k == 3 ? s.mod : s.fl;
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
  http.send(404, "text/plain", "Not found. This node only answers /, /api/status, /api/log, /api/history and /log.txt\r\n");
}

// ---------- task ----------
static void web_task(void *) {
  // Bluetooth first: the display gets up to 20 s to open its session before Wi-Fi takes radio time.
  while (millis() < 20000 && bleStateNow != 2) vTaskDelay(pdMS_TO_TICKS(200));

  WiFi.setHostname(NODE_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);                                  // must stay on while Bluetooth is in use
  WiFi.setAutoReconnect(true);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);            // look at every access point with this name...
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);        // ...and join the strongest
  WiFi.begin(NODE_WIFI_SSID, NODE_WIFI_PASS);
  nlog("Wi-Fi: joining %s", NODE_WIFI_SSID);

  http.on("/", HTTP_GET, h_root);
  http.on("/api/status", HTTP_GET, h_status);
  http.on("/api/log", HTTP_GET, h_log);
  http.on("/api/history", HTTP_GET, h_history);
  http.on("/log.txt", HTTP_GET, h_logtxt);
  http.onNotFound(h_notfound);

  bool up = false, serving = false, clockSet = false;
  uint32_t lastTryMs = millis(), downSinceMs = millis();
  for (;;) {
    uint32_t now = millis();
    bool isUp = WiFi.status() == WL_CONNECTED;
    if (isUp != up) {
      up = isUp;
      if (up) {
        nlog("Wi-Fi up: %s, %d dBm, channel %d", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (int)WiFi.channel());
        if (!serving) {
          http.begin();
          serving = true;
          configTzTime(NODE_TZ, "pool.ntp.org", "time.nist.gov");
          nlog("status page at http://%s/", WiFi.localIP().toString().c_str());
        }
      } else {
        nlog("Wi-Fi lost");
        downSinceMs = lastTryMs = now;
      }
    }
    if (!up && now - lastTryMs > 30000) {
      lastTryMs = now;
      if (now - downSinceMs > 60000) nlog("Wi-Fi still down after %lu s, trying again", (unsigned long)((now - downSinceMs) / 1000));
      WiFi.reconnect();
    }
    if (!clockSet && clock_now()) { clockSet = true; nlog("clock set from the network"); }

    if (serving && up) http.handleClient();
    vTaskDelay(pdMS_TO_TICKS(serving && up ? 2 : 100));
  }
}

void web_begin(const char *build, int reason, const SafetyParams *lim) {
  buildName = build;
  resetReason = reason;
  limits = *lim;
  bootId = esp_random() | 1;
  // Same priority as the main loop: the two share the processor in turns, neither can starve the other.
  // This task is not registered with the watchdog; only a hung main loop restarts the node.
  xTaskCreate(web_task, "web", 8192, NULL, 1, NULL);
}

#endif   // NODE_WEB

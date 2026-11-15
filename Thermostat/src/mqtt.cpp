// MQTT with Home Assistant discovery. Runs in its own FreeRTOS task so a slow or absent broker
// can never stall the display. Thread rule (see shared.h): this task never touches st. It reads a
// published snapshot and posts commands for the main task to apply.
//
// Topics (base cv/thermostat):
//   status                 online / offline (LWT, retained)
//   state                  JSON, when something changes and once a minute
//   diag                   JSON, every 30 s (health, link quality, signal levels)
//   set/target             number  -> target temperature
//   set/mode               off | heat | auto
//   set/outside            number  -> outdoor temperature (heating curve + top bar)
//
// TLS: set MQTT_TLS 1 and MQTT_CA_CERT (PEM of the broker's CA) in secrets.h, use port 8883.

#include "state.h"
#include "shared.h"
#include "evlog.h"
#include "secrets.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

#ifndef MQTT_TLS
#define MQTT_TLS 0
#endif
#if MQTT_TLS
#include <WiFiClientSecure.h>
static WiFiClientSecure wc;
#else
static WiFiClient wc;
#endif

#define T_BASE   "cv/thermostat"
#define T_AVAIL  T_BASE "/status"
#define T_STATE  T_BASE "/state"
#define T_DIAG   T_BASE "/diag"
#define T_SET    T_BASE "/set/"
#define DEV_FULL "\"dev\":{\"ids\":[\"cv_thermostat\"],\"name\":\"CV Thermostat\",\"mf\":\"DIY\",\"mdl\":\"ESP32-S3 display + OpenTherm node\"}"

static PubSubClient mq(wc);
static bool discOk = true;                  // false if any discovery message could not be sent
static ThermoState s;                       // this task's private copy of the state

static const char *mode_str() { return s.mode == MODE_OFF ? "off" : (s.mode == MODE_AUTO ? "auto" : "heat"); }
static const char *action_str() { return s.heating ? "heating" : (s.mode == MODE_OFF ? "off" : "idle"); }

static void on_message(char *topic, byte *payload, unsigned int len) {
  char v[24];
  if (len >= sizeof(v)) len = sizeof(v) - 1;
  memcpy(v, payload, len); v[len] = 0;
  const char *t = topic + strlen(T_SET);
  char *end;
  float f = strtof(v, &end);
  bool isNum = end != v;                              // "unknown" / "unavailable" from HA are not numbers
  if (!strcmp(t, "target")) {
    if (isNum) shared_post(CMD_SET_TARGET, f);
  } else if (!strcmp(t, "mode")) {
    if (!strcmp(v, "off")) shared_post(CMD_SET_MODE, MODE_OFF);
    else if (!strcmp(v, "heat")) shared_post(CMD_SET_MODE, MODE_MANUAL);
    else if (!strcmp(v, "auto")) shared_post(CMD_SET_MODE, MODE_AUTO);
  } else if (!strcmp(t, "outside")) {
    if (isNum) shared_post(CMD_SET_OUTSIDE, f);
  }
}

// kind: "sensor" or "binary_sensor". stateTopic: T_STATE or T_DIAG. diag: mark as diagnostic entity.
static void disc(const char *kind, const char *id, const char *name, const char *stateTopic, const char *key,
                 const char *unit, const char *dclass, bool diag) {
  static char topic[112], msg[720];                   // static: keep them off the task stack
  snprintf(topic, sizeof(topic), "homeassistant/%s/cv_thermostat/%s/config", kind, id);
  int n = snprintf(msg, sizeof(msg),
    "{\"name\":\"%s\",\"uniq_id\":\"cv_thermostat_%s\",\"stat_t\":\"%s\",\"val_tpl\":\"{{ value_json.%s }}\","
    "\"avty_t\":\"" T_AVAIL "\"", name, id, stateTopic, key);
  if (unit)   n += snprintf(msg + n, sizeof(msg) - n, ",\"unit_of_meas\":\"%s\",\"stat_cla\":\"measurement\"", unit);
  if (dclass) n += snprintf(msg + n, sizeof(msg) - n, ",\"dev_cla\":\"%s\"", dclass);
  if (diag)   n += snprintf(msg + n, sizeof(msg) - n, ",\"ent_cat\":\"diagnostic\"");
  snprintf(msg + n, sizeof(msg) - n, "," DEV_FULL "}");
  if (!mq.publish(topic, msg, true)) discOk = false;
}

static void publish_discovery() {
  static const char climate[] =
    "{\"name\":null,\"uniq_id\":\"cv_thermostat_climate\","
    "\"modes\":[\"off\",\"heat\",\"auto\"],"
    "\"mode_stat_t\":\"" T_STATE "\",\"mode_stat_tpl\":\"{{ value_json.mode }}\","
    "\"mode_cmd_t\":\"" T_SET "mode\","
    "\"temp_stat_t\":\"" T_STATE "\",\"temp_stat_tpl\":\"{{ value_json.target }}\","
    "\"temp_cmd_t\":\"" T_SET "target\","
    "\"curr_temp_t\":\"" T_STATE "\",\"curr_temp_tpl\":\"{{ value_json.room }}\","
    "\"act_t\":\"" T_STATE "\",\"act_tpl\":\"{{ value_json.action }}\","
    "\"min_temp\":5,\"max_temp\":30,\"temp_step\":0.5,\"temp_unit\":\"C\","
    "\"avty_t\":\"" T_AVAIL "\"," DEV_FULL "}";
  discOk = mq.publish("homeassistant/climate/cv_thermostat/config", climate, true);

  disc("sensor", "humidity",      "Humidity",          T_STATE, "hum",      "%",   "humidity", false);
  disc("sensor", "flow",          "Radiator water",    T_STATE, "flow",     "°C",  "temperature", false);
  disc("sensor", "return",        "Return water",      T_STATE, "ret",      "°C",  "temperature", false);
  disc("sensor", "dhw",           "Hot water",         T_STATE, "dhw",      "°C",  "temperature", false);
  disc("sensor", "pressure",      "Boiler pressure",   T_STATE, "pressure", "bar", "pressure", false);
  disc("sensor", "modulation",    "Modulation",        T_STATE, "mod",      "%",   NULL, false);
  disc("sensor", "flow_request",  "Flow request",      T_STATE, "flow_req", "°C",  "temperature", false);
  disc("sensor", "fault_code",    "Boiler fault code", T_STATE, "fault",    NULL,  NULL, false);
  disc("sensor", "wifi_rssi",     "Wi-Fi signal",      T_DIAG,  "rssi",     "dBm", "signal_strength", true);
  disc("binary_sensor", "flame",        "Flame",        T_STATE, "flame",     NULL, "heat", false);
  disc("binary_sensor", "boiler_link",  "Boiler link",  T_STATE, "link",      NULL, "connectivity", false);
  disc("binary_sensor", "boiler_fault", "Boiler fault", T_STATE, "has_fault", NULL, "problem", false);

  // things a human should be told about
  disc("binary_sensor", "fallback",     "Boiler node in fallback", T_DIAG, "fallback",     NULL, "problem", false);
  disc("binary_sensor", "sensor_fault", "Room sensor fault",       T_DIAG, "sensor_fault", NULL, "problem", false);
  disc("binary_sensor", "frost",        "Frost protection active", T_DIAG, "frost",        NULL, "cold", false);
  disc("binary_sensor", "preheat",      "Preheating",              T_DIAG, "preheat",      NULL, NULL, false);
  disc("binary_sensor", "safe_mode",    "Safe mode",               T_DIAG, "safe_mode",    NULL, "problem", true);

  // health
  disc("sensor", "ble_rssi",       "Boiler link signal",       T_DIAG, "ble_rssi",   "dBm", "signal_strength", true);
  disc("sensor", "ble_reconnects", "Boiler link reconnects",   T_DIAG, "ble_reconn", NULL,  NULL, true);
  disc("sensor", "uptime",         "Uptime",                   T_DIAG, "uptime",     "s",   "duration", true);
  disc("sensor", "heap_min",       "Free memory minimum",      T_DIAG, "heap_min",   "B",   "data_size", true);
  disc("sensor", "reset_reason",   "Last reset reason",        T_DIAG, "reset",      NULL,  NULL, true);
  disc("sensor", "crash_count",    "Crash count",              T_DIAG, "crashes",    NULL,  NULL, true);
  disc("sensor", "heat_rate",      "Learned warm-up rate",     T_DIAG, "heat_rate",  "K/h", NULL, true);
  disc("sensor", "outside_used",   "Outdoor temperature used", T_DIAG, "outside",    "°C",  "temperature", true);
}

static void fnum(char *out, size_t n, bool ok, const char *fmt, float v) {
  if (ok) snprintf(out, n, fmt, v); else snprintf(out, n, "null");
}

// The state message is sent when its content changes, and once a minute regardless so Home Assistant
// can tell a quiet thermostat from a dead one. Signal levels live in the diag message because they
// jitter and would otherwise force a publish every second.
static bool publish_state(bool force) {
  static char msg[520], sent[520];
  char room[12], hum[12], flow[12], ret[12], dhw[12], pres[12], freq[12];
  bool b = s.bleLinked && s.boilerLinked;
  fnum(room, sizeof(room), s.sensorOk, "%.1f", s.room);
  fnum(hum,  sizeof(hum),  s.sensorOk, "%.0f", s.humidity);
  fnum(flow, sizeof(flow), b, "%.1f", s.flow);
  fnum(ret,  sizeof(ret),  b, "%.1f", s.ret);
  fnum(dhw,  sizeof(dhw),  b, "%.1f", s.dhw);
  fnum(pres, sizeof(pres), b, "%.2f", s.pressure);
  fnum(freq, sizeof(freq), s.heating, "%.1f", s.flowRequest);
  snprintf(msg, sizeof(msg),
    "{\"room\":%s,\"hum\":%s,\"target\":%.1f,\"mode\":\"%s\",\"action\":\"%s\","
    "\"flow\":%s,\"ret\":%s,\"dhw\":%s,\"pressure\":%s,\"mod\":%u,\"flow_req\":%s,"
    "\"flame\":\"%s\",\"link\":\"%s\",\"has_fault\":\"%s\",\"fault\":\"%s\"}",
    room, hum, s.target, mode_str(), action_str(),
    flow, ret, dhw, pres, (unsigned)((b && s.flame) ? s.modulation : 0), freq,
    (b && s.flame) ? "ON" : "OFF", s.bleLinked ? "ON" : "OFF", s.boilerFault ? "ON" : "OFF",
    s.boilerFault ? s.lastFault : "none");
  if (!force && strcmp(msg, sent) == 0) return false;
  if (!mq.publish(T_STATE, msg, false)) return false;
  strcpy(sent, msg);
  return true;
}

static void publish_diag() {
  static char msg[460];
  char out[12];
  fnum(out, sizeof(out), s.outsideOk, "%.1f", s.outside);
  snprintf(msg, sizeof(msg),
    "{\"fallback\":\"%s\",\"sensor_fault\":\"%s\",\"frost\":\"%s\",\"preheat\":\"%s\",\"safe_mode\":\"%s\","
    "\"ble_rssi\":%d,\"ble_reconn\":%lu,\"uptime\":%lu,\"heap_min\":%u,\"reset\":\"%s\",\"crashes\":%u,"
    "\"heat_rate\":%.2f,\"outside\":%s,\"rssi\":%d}",
    (s.bleLinked && s.boilerFallback) ? "ON" : "OFF", s.sensorFault ? "ON" : "OFF",
    (s.frostActive || s.nodeFrost) ? "ON" : "OFF", s.preheat ? "ON" : "OFF", s.safeMode ? "ON" : "OFF",
    s.bleLinked ? s.bleRssi : 0, (unsigned long)s.bleReconnects, (unsigned long)s.uptimeS,
    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), s.resetReason, s.crashCount,
    s.heatRate, out, s.wifiRssi);
  mq.publish(T_DIAG, msg, false);
}

static void mqtt_task(void *) {
  uint32_t lastTry = 0, lastState = 0, lastDiag = 0, lastDisc = 0;
  bool everTried = false, diagSent = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(50));
    shared_snapshot(&s);
    if (!s.wifiOk) { g_mqttConnected = false; continue; }
    uint32_t now = millis();
    if (!mq.connected()) {
      g_mqttConnected = false;
      if (everTried && now - lastTry < 10000) continue;
      everTried = true;
      lastTry = now;
      if (mq.connect("cv-thermostat", MQTT_USER, MQTT_PASS, T_AVAIL, 0, true, "offline")) {
        mq.publish(T_AVAIL, "online", true);
        mq.subscribe(T_SET "#");
        publish_discovery();
        lastDisc = now;
        lastState = now - 61000;                 // forces a state message right away
        diagSent = false;
        elog("%s", discOk ? "MQTT connected, discovery sent" : "MQTT connected, some discovery messages failed, will retry");
      } else {
        { static int lastRc = 0; static uint32_t fails = 0; int rc = mq.state(); fails++;      // a broker that stays away must not fill the log
          if (rc != lastRc || fails % 30 == 1) elog("MQTT connect failed, rc=%d (attempt %lu)", rc, (unsigned long)fails);
          lastRc = rc; }
      }
      continue;
    }
    g_mqttConnected = true;
    mq.loop();
    if (!discOk && now - lastDisc > 30000) { lastDisc = now; publish_discovery(); }
    if (publish_state(now - lastState > 60000)) lastState = now;
    if (!diagSent || now - lastDiag > 30000) { diagSent = true; lastDiag = now; publish_diag(); }
  }
}

void mqtt_init() {
#if MQTT_TLS
  wc.setCACert(MQTT_CA_CERT);                 // only a broker presenting a certificate from this CA is accepted
#endif
  wc.setTimeout(3);
  mq.setServer(MQTT_HOST, MQTT_PORT);
  if (!mq.setBufferSize(1536)) Serial.println("MQTT buffer allocation failed");   // must happen before connecting
  mq.setKeepAlive(30);
  mq.setCallback(on_message);
  xTaskCreatePinnedToCore(mqtt_task, "mqtt", 8192, NULL, 1, NULL, 0);
}

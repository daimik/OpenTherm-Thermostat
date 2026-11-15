#include "evlog.h"
#include <Arduino.h>
#include <stdarg.h>
#include <string.h>
#include "esp_heap_caps.h"

static EvLine *ring = nullptr;
static uint32_t newest = 0;                                   // line n lives in ring[n % EVLOG_LINES]
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;       // held only for one small copy

void evlog_init() {
  if (ring) return;
  ring = (EvLine *)heap_caps_calloc(EVLOG_LINES, sizeof(EvLine), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void elog(const char *fmt, ...) {
  char buf[EVLOG_TEXT];
  va_list a;
  va_start(a, fmt);
  vsnprintf(buf, sizeof(buf), fmt, a);
  va_end(a);
  Serial.println(buf);
  if (!ring) return;
  uint32_t ms = millis();
  portENTER_CRITICAL(&mux);
  EvLine &l = ring[(newest + 1) % EVLOG_LINES];
  l.seq = ++newest;
  l.ms = ms;
  memcpy(l.text, buf, sizeof(l.text));
  portEXIT_CRITICAL(&mux);
}

uint32_t evlog_newest() { return newest; }
uint32_t evlog_oldest() {
  uint32_t n = newest;
  return n == 0 ? 0 : (n > EVLOG_LINES ? n - EVLOG_LINES + 1 : 1);
}
bool evlog_get(uint32_t seq, EvLine *out) {
  if (!ring || seq == 0) return false;
  bool ok = false;
  portENTER_CRITICAL(&mux);
  const EvLine &l = ring[seq % EVLOG_LINES];
  if (l.seq == seq) { *out = l; ok = true; }
  portEXIT_CRITICAL(&mux);
  return ok;
}

// ---------- state changes ----------
static const char *mode_name(ThermoMode m) { return m == MODE_AUTO ? "Auto" : (m == MODE_MANUAL ? "Manual" : "Off"); }

void evlog_state_tick(const ThermoState &s) {
  static bool first = true;
  static ThermoState p;                        // the state one second ago
  static float loggedTarget = -100, pendingTarget = -100;
  static uint8_t targetStable = 0;
  if (first) {
    first = false;
    p = s;
    loggedTarget = pendingTarget = s.target;
    elog("state: mode %s, target %.1f, room %s", mode_name(s.mode), s.target, s.sensorOk ? "sensor ok" : "sensor not ready");
    return;
  }

  if (s.mode != p.mode) elog("mode: %s -> %s", mode_name(p.mode), mode_name(s.mode));

  // The dial passes through many values while it is dragged: a target is logged once it has stood for 2 s.
  if (s.target != pendingTarget) { pendingTarget = s.target; targetStable = 0; }
  else if (targetStable < 250) targetStable++;
  if (pendingTarget != loggedTarget && targetStable >= 2 && s.mode != MODE_OFF) {
    loggedTarget = pendingTarget;
    if (s.mode == MODE_AUTO && s.autoActive && s.preheat)
      elog("target %.1f: preheating for the block at %02u:%02u", s.target, s.nextChangeMin / 60, s.nextChangeMin % 60);
    else if (s.mode == MODE_AUTO && s.autoActive && !s.overrideActive)
      elog("target %.1f: schedule, until %02u:%02u", s.target, s.nextChangeMin / 60, s.nextChangeMin % 60);
    else if (s.mode == MODE_AUTO && s.overrideActive)
      elog("target %.1f: set by hand, holds until %02u:%02u", s.target, s.nextChangeMin / 60, s.nextChangeMin % 60);
    else
      elog("target %.1f: set by hand", s.target);
  }

  if (s.heating != p.heating) {
    if (s.heating) elog("heating on: room %.1f, target %.1f, asking flow %.1f", s.room, s.target, s.flowRequest);
    else           elog("heating off: room %.1f, target %.1f", s.room, s.target);
  }
  if (s.frostActive != p.frostActive) elog("%s", s.frostActive ? "FROST GUARD on: room below the frost limit while Off" : "frost guard off");
  if (s.sensorFault != p.sensorFault) {
    if (s.sensorFault) elog("SENSOR FAULT: %s", s.sensorPresent ? "no plausible room temperature for 30 s" : "room sensor does not answer");
    else               elog("room sensor ok: %.1f, %.0f %%", s.room, s.humidity);
  }
  if (s.outsideOk != p.outsideOk) {
    if (s.outsideOk) elog("outdoor temperature available: %.1f", s.outside);
    else             elog("outdoor temperature too old: the heating curve uses its fixed base");
  }
  if (s.clockOk != p.clockOk) elog("%s", s.clockOk ? "clock ok" : "clock lost: Auto holds the current target");

  // What the boiler node reports. Its first status arrives about a second after the link opens, so the
  // boiler's state is logged once at the third second and compared from then on.
  static uint8_t linkAge = 0;
  if (!s.bleLinked) linkAge = 0;
  else if (linkAge < 250) linkAge++;
  if (linkAge == 3) elog("node: boiler %s", s.boilerSimulated ? "simulated" : (s.boilerLinked ? "answering" : "SILENT (no OpenTherm answer)"));
  if (linkAge > 3) {
    if (s.boilerLinked != p.boilerLinked) elog("%s", s.boilerLinked ? "node: boiler answering" : "node: BOILER LINK LOST (OpenTherm silent)");
    if (s.boilerFallback != p.boilerFallback) elog("%s", s.boilerFallback ? "node: running on FALLBACK" : "node: following the display again");
    if (s.nodeFrost != p.nodeFrost) elog("%s", s.nodeFrost ? "node: FROST GUARD on (cold water in the boiler)" : "node: frost guard off");
    if (s.nodeClamped && !p.nodeClamped) elog("node: requested flow %.1f was limited", s.flowRequest);
    if (s.nodeHold && !p.nodeHold) elog("node: hold, a heating on/off change waits for the minimum time");
    if (s.flame != p.flame) {
      if (s.flame) elog("flame on: flow %.1f, return %.1f", s.flow, s.ret);
      else         elog("flame off: flow %.1f, return %.1f", s.flow, s.ret);
    }
  }
  if (s.boilerFault != p.boilerFault || (s.boilerFault && strcmp(s.lastFault, p.lastFault) != 0)) {
    if (s.boilerFault) elog("BOILER FAULT %s", s.lastFault);
    else               elog("boiler fault cleared");
  }

  if (s.haOk != p.haOk && !s.haOk) elog("MQTT lost");
  p = s;
}

// Boiler node safety layer. Pure logic, no Arduino: the same code runs in the firmware and in PC tests.
// Whatever the display asks for passes through here before it reaches the boiler:
//   - fallback when no valid control arrives
//   - setpoint clamped to a fixed range
//   - frost guard on return-water temperature, independent of the display
//   - minimum on / off times so nothing can short-cycle the burner
#pragma once
#include <stdint.h>
#include "ble_protocol.h"

struct SafetyParams {
  int16_t  flowMin_c10 = 200;        // 20 °C
  int16_t  flowMax_c10 = 750;        // 75 °C: radiators. Lower this for floor heating.
  uint32_t minOnMs  = 180000;        // 3 min
  uint32_t minOffMs = 180000;        // 3 min
  int16_t  frostOn_c10  = 80;        // return water below 8 °C: heat
  int16_t  frostOff_c10 = 150;       // until it is above 15 °C
  int16_t  frostFlow_c10 = 350;
  int16_t  fallbackFlow_c10 = 450;   // no control frames: 45 °C fixed
  uint32_t fallbackMs = 600000;      // 10 min
};

struct SafetyState {
  bool     chOn = false;
  bool     everSwitched = false;
  uint32_t lastSwitchMs = 0;
  bool     frost = false;
  int16_t  lastFlow_c10 = 300;
};

struct SafetyIn {
  uint32_t nowMs;
  bool     haveControl;      // at least one valid control frame since boot
  uint32_t controlAgeMs;     // age of the newest valid control frame
  bool     reqCh;
  int16_t  reqFlow_c10;
  bool     otLink;           // boiler values are valid
  int16_t  return_c10;
};

struct SafetyOut {
  bool    ch;
  int16_t flow_c10;
  bool    fallback, frost, clamped, hold;
};

static inline SafetyOut ns_apply(SafetyState &s, const SafetyIn &in, const SafetyParams &p) {
  SafetyOut o = { false, p.flowMin_c10, false, false, false, false };
  bool wantCh; int16_t flow;

  o.fallback = !in.haveControl || in.controlAgeMs > p.fallbackMs;
  if (o.fallback) { wantCh = true; flow = p.fallbackFlow_c10; }
  else            { wantCh = in.reqCh; flow = in.reqFlow_c10; }

  if (wantCh) {
    if (flow < p.flowMin_c10) { flow = p.flowMin_c10; o.clamped = true; }
    if (flow > p.flowMax_c10) { flow = p.flowMax_c10; o.clamped = true; }
  }

  if (in.otLink) {
    if (!s.frost && in.return_c10 < p.frostOn_c10) s.frost = true;
    if (s.frost && in.return_c10 > p.frostOff_c10) s.frost = false;
  }
  if (s.frost) {
    o.frost = !wantCh || flow < p.frostFlow_c10;
    if (!wantCh) { wantCh = true; flow = p.frostFlow_c10; }
    else if (flow < p.frostFlow_c10) flow = p.frostFlow_c10;
  }

  if (wantCh != s.chOn) {
    uint32_t need = s.chOn ? p.minOnMs : p.minOffMs;
    // frost protection may switch on immediately; it must still respect minimum on time when ending
    bool urgent = wantCh && s.frost;
    if (!s.everSwitched || urgent || (uint32_t)(in.nowMs - s.lastSwitchMs) >= need) {
      s.chOn = wantCh; s.everSwitched = true; s.lastSwitchMs = in.nowMs;
    } else {
      o.hold = true;
    }
  }
  if (s.chOn && wantCh) s.lastFlow_c10 = flow;
  o.ch = s.chOn;
  o.flow_c10 = s.chOn ? s.lastFlow_c10 : p.flowMin_c10;
  return o;
}

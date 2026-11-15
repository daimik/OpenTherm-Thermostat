// Thermostat logic: room temperature + target -> heating enable + flow setpoint for the boiler node.
//
//   normal    : hysteresis on demand, PI controller with anti-windup on a weather-compensated base
//   Off       : frost guard only
//   no sensor : a low fixed flow so the house does not go cold (the node clamps and frost-guards too)
//
// The control law itself is tl_control() in include/thermo_logic.h and is unit-tested on the PC.
// This file feeds it, learns the room's warm-up rate, and ships the result over BLE.

#include "control.h"
#include "state.h"
#include "ble_link.h"
#include "thermo_logic.h"
#include <Arduino.h>

static const CtrlParams params = {};
static CtrlState cs;
static BoilerControl last = {};
static uint32_t lastSendMs = 0;

// warm-up rate learning
static float rateRefTemp = 0;
static uint32_t rateRefMs = 0;
static bool rateTracking = false;

static void learn_rate(bool regulating) {
  uint32_t now = millis();
  if (!regulating || !st.sensorOk) { rateTracking = false; return; }
  if (!rateTracking) { rateTracking = true; rateRefTemp = st.room; rateRefMs = now; return; }
  if (now - rateRefMs >= 600000UL) {                      // every 10 min of uninterrupted heating
    float rate = (st.room - rateRefTemp) * 6.0f;          // K per hour
    if (rate > 0.3f && rate < 3.0f) st.heatRate = 0.8f * st.heatRate + 0.2f * rate;
    rateRefTemp = st.room;
    rateRefMs = now;
  }
}

void control_tick() {
  // an outdoor value older than 2 h is no longer trusted for the heating curve
  if (st.outsideOk && millis() - st.outsideMs > 7200000UL) st.outsideOk = false;

  CtrlIn in;
  in.modeOff = st.mode == MODE_OFF;
  in.sensorOk = st.sensorOk;
  in.room = st.room;
  in.target = st.target;
  in.outsideOk = st.outsideOk;
  in.outside = st.outside;
  in.dt = 1.0f;
  CtrlOut out = tl_control(cs, in, params);

  BoilerControl c = {};
  c.dhw_setpoint_c10 = 0;                      // leave the boiler's own hot-water setting alone
  c.flags = CF_DHW_ENABLE;
  if (out.chEnable) {
    c.flags |= CF_CH_ENABLE;
    c.ch_setpoint_c10 = (int16_t)lroundf(out.flow * 10);
  } else {
    c.ch_setpoint_c10 = 300;                   // parked low; ignored while heating is disabled
  }

  st.heating = out.chEnable;
  st.frostActive = out.frost;
  st.flowRequest = out.chEnable ? out.flow : 0;
  st.ctrlDemand = cs.on;                       // for the status page
  st.ctrlBase = st.outsideOk ? tl_weather_base(st.outside) : params.baseNoWeather;
  st.ctrlInteg = cs.integ;
  learn_rate(cs.on);

  // send on a meaningful change (0.5 °C or a flag) and at least every 30 s to keep the node out of fallback
  bool changed = c.flags != last.flags || abs(c.ch_setpoint_c10 - last.ch_setpoint_c10) >= 5;
  uint32_t now = millis();
  if (changed || now - lastSendMs > 30000) {
    ble_link_send_control(c);
    last = c;
    lastSendMs = now;
  }
}

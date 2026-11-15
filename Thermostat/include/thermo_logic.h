// Pure thermostat logic: schedule lookup, preheat, and the control law.
// No Arduino, no globals, so the same code runs in the firmware and in the PC unit tests (tests/).
#pragma once
#include <stdint.h>
#include <math.h>

// ---------------- schedule ----------------
#define SCHED_BLOCKS 4

struct SchedBlock {
  uint16_t startMin;    // minutes since midnight
  int16_t  temp_c10;    // 0.1 °C
};

struct ThermoSchedule {
  SchedBlock weekday[SCHED_BLOCKS];
  SchedBlock weekend[SCHED_BLOCKS];
};

struct ActiveBlock {
  int      key;             // unique per (day type, block): changes exactly when a new block starts
  int16_t  temp_c10;
  uint16_t nextStartMin;    // minutes since midnight when the next block starts
  int16_t  nextTemp_c10;
  uint16_t minutesToNext;
};

// dow: 0 = Monday .. 6 = Sunday. Before the first block of a day, yesterday's last block applies.
static inline ActiveBlock tl_active_block(const ThermoSchedule &s, uint8_t dow, uint16_t minute) {
  ActiveBlock a;
  bool weekend = dow >= 5;
  const SchedBlock *day = weekend ? s.weekend : s.weekday;
  int idx = -1;
  for (int i = 0; i < SCHED_BLOCKS; i++) if (minute >= day[i].startMin) idx = i;
  if (idx >= 0) {
    a.key = (weekend ? 10 : 0) + idx;
    a.temp_c10 = day[idx].temp_c10;
    if (idx + 1 < SCHED_BLOCKS) {
      a.nextStartMin = day[idx + 1].startMin;
      a.nextTemp_c10 = day[idx + 1].temp_c10;
      a.minutesToNext = (uint16_t)(a.nextStartMin - minute);
    } else {
      uint8_t tomorrow = (uint8_t)((dow + 1) % 7);
      const SchedBlock *t = tomorrow >= 5 ? s.weekend : s.weekday;
      a.nextStartMin = t[0].startMin;
      a.nextTemp_c10 = t[0].temp_c10;
      a.minutesToNext = (uint16_t)(1440 - minute + a.nextStartMin);
    }
  } else {
    uint8_t yesterday = (uint8_t)((dow + 6) % 7);
    bool yWeekend = yesterday >= 5;
    const SchedBlock *y = yWeekend ? s.weekend : s.weekday;
    a.key = (yWeekend ? 10 : 0) + SCHED_BLOCKS - 1;
    a.temp_c10 = y[SCHED_BLOCKS - 1].temp_c10;
    a.nextStartMin = day[0].startMin;
    a.nextTemp_c10 = day[0].temp_c10;
    a.minutesToNext = (uint16_t)(a.nextStartMin - minute);
  }
  return a;
}

// Minutes of lead time needed to raise the room by deltaK at the learned heating rate. Capped at 2 h.
static inline uint16_t tl_preheat_minutes(float deltaK, float rateKperHour) {
  if (deltaK <= 0.2f) return 0;
  if (rateKperHour < 0.3f) rateKperHour = 0.3f;
  float m = deltaK / rateKperHour * 60.0f;
  if (m > 120.0f) m = 120.0f;
  return (uint16_t)lroundf(m);
}

// ---------------- control law ----------------
struct CtrlParams {
  float hystOn = 0.15f, hystOff = 0.25f;   // K below / above target
  float kp = 12.0f;                         // °C flow per K error
  float ki = 0.01f;                         // °C flow per (K * s)
  float iMax = 15.0f;                       // integrator limit, °C flow
  float flowMin = 30.0f, flowMax = 70.0f;
  float baseNoWeather = 38.0f;              // flow base when no outdoor temperature is known
  float frostOn = 6.0f, frostOff = 7.5f;    // Off mode: room frost guard band
  float frostFlow = 40.0f;
  float sensorFaultFlow = 40.0f;            // modest fixed flow when the room sensor is unusable
};

struct CtrlState {
  bool  on = false;       // heat demand (with hysteresis)
  bool  frost = false;    // frost guard latched
  float integ = 0.0f;
};

struct CtrlIn {
  bool  modeOff;
  bool  sensorOk;
  float room, target;
  bool  outsideOk;
  float outside;
  float dt;               // seconds since last call
};

struct CtrlOut {
  bool  chEnable;
  float flow;             // requested flow temperature, °C (valid when chEnable)
  bool  frost;            // heating only because of the frost guard
  bool  sensorFault;      // heating blind at sensorFaultFlow
};

static inline float tl_clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Heating curve: mild weather needs cooler radiators. 15 °C outside -> 30, 0 -> 51, -10 -> 65.
static inline float tl_weather_base(float outside) {
  return tl_clamp(30.0f + 1.4f * (15.0f - outside), 30.0f, 65.0f);
}

static inline CtrlOut tl_control(CtrlState &s, const CtrlIn &in, const CtrlParams &p) {
  CtrlOut o = { false, p.flowMin, false, false };

  if (!in.sensorOk) {
    // Without a room temperature there is nothing to regulate on. Off stays off (the boiler node has
    // its own frost guard on return water); otherwise heat gently rather than not at all.
    s.on = false; s.frost = false;
    if (!in.modeOff) { o.chEnable = true; o.flow = p.sensorFaultFlow; o.sensorFault = true; }
    return o;
  }

  if (in.modeOff) {
    s.on = false; s.integ = 0;
    if (!s.frost && in.room < p.frostOn) s.frost = true;
    if (s.frost && in.room > p.frostOff) s.frost = false;
    if (s.frost) { o.chEnable = true; o.flow = p.frostFlow; o.frost = true; }
    return o;
  }
  s.frost = false;

  float err = in.target - in.room;
  if (!s.on && err > p.hystOn) s.on = true;
  if (s.on && err < -p.hystOff) s.on = false;
  if (!s.on) return o;

  float base = in.outsideOk ? tl_weather_base(in.outside) : p.baseNoWeather;
  float raw = base + p.kp * err + s.integ;
  // anti-windup: only integrate while the output is not pinned in the direction of the error
  bool satHigh = raw >= p.flowMax && err > 0;
  bool satLow  = raw <= p.flowMin && err < 0;
  if (!satHigh && !satLow) s.integ = tl_clamp(s.integ + p.ki * err * in.dt, -p.iMax, p.iMax);
  o.chEnable = true;
  o.flow = tl_clamp(base + p.kp * err + s.integ, p.flowMin, p.flowMax);
  return o;
}

// ---------------- sensor plausibility ----------------
struct SensorFilter {
  bool    have = false;
  float   value = 0;
  uint8_t rejects = 0;
};

// Returns true if the sample was accepted into f.value. A reading outside the physical range is
// always refused. A jump of more than maxStep from the filtered value is refused up to 3 times,
// then believed (the room really did change, e.g. a window was opened next to the sensor).
static inline bool tl_sensor_accept(SensorFilter &f, float sample, float lo, float hi, float maxStep, float alpha) {
  if (!(sample >= lo && sample <= hi)) return false;          // also rejects NaN
  if (!f.have) { f.have = true; f.value = sample; f.rejects = 0; return true; }
  if (fabsf(sample - f.value) > maxStep && f.rejects < 3) { f.rejects++; return false; }
  f.rejects = 0;
  f.value += (sample - f.value) * alpha;
  return true;
}

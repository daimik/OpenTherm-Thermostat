#include <cstdio>
#include "thermo_logic.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, e) (fabs((a) - (b)) <= (e))

int main() {
  ThermoSchedule s = {
    { {390, 205}, {510, 170}, {1020, 210}, {1350, 165} },     // 06:30 08:30 17:00 22:30
    { {480, 205}, {600, 200}, {1020, 210}, {1380, 165} },     // 08:00 10:00 17:00 23:00
  };
  // --- schedule ---
  ActiveBlock a = tl_active_block(s, 1, 9 * 60);                   // Tue 09:00 -> Away
  CHECK(a.key == 1 && a.temp_c10 == 170 && a.nextStartMin == 1020 && a.nextTemp_c10 == 210 && a.minutesToNext == 480);
  a = tl_active_block(s, 1, 5 * 60);                               // Tue 05:00 -> Mon night
  CHECK(a.key == 3 && a.temp_c10 == 165 && a.nextStartMin == 390 && a.minutesToNext == 90);
  a = tl_active_block(s, 5, 7 * 60);                               // Sat 07:00 -> Fri (weekday) night
  CHECK(a.key == 3 && a.temp_c10 == 165 && a.nextStartMin == 480 && a.nextTemp_c10 == 205);
  a = tl_active_block(s, 0, 7 * 60);                               // Mon 07:00 -> Wake (weekday)
  CHECK(a.key == 0 && a.temp_c10 == 205);
  a = tl_active_block(s, 0, 3 * 60);                               // Mon 03:00 -> Sun (weekend) night
  CHECK(a.key == 13 && a.temp_c10 == 165);
  a = tl_active_block(s, 4, 23 * 60);                              // Fri 23:00 -> night, next = Sat 08:00
  CHECK(a.key == 3 && a.nextStartMin == 480 && a.minutesToNext == 60 + 480);
  a = tl_active_block(s, 6, 23 * 60 + 30);                         // Sun 23:30 -> weekend night, next = Mon 06:30
  CHECK(a.key == 13 && a.nextStartMin == 390 && a.minutesToNext == 30 + 390);
  a = tl_active_block(s, 2, 1020);                                 // exactly at block start
  CHECK(a.key == 2 && a.temp_c10 == 210);

  CHECK(tl_preheat_minutes(0.1f, 1.0f) == 0);
  CHECK(tl_preheat_minutes(2.0f, 1.0f) == 120);
  CHECK(tl_preheat_minutes(1.0f, 2.0f) == 30);
  CHECK(tl_preheat_minutes(5.0f, 1.0f) == 120);                    // capped
  CHECK(tl_preheat_minutes(1.0f, 0.0f) == 120);                    // silly rate is floored, then capped

  // --- control ---
  CtrlParams p; CtrlState st; CtrlIn in = { false, true, 20.0f, 21.0f, false, 0, 1.0f };
  CtrlOut o = tl_control(st, in, p);
  CHECK(o.chEnable && NEAR(o.flow, 38 + 12 * 1.0 + 0.01, 0.05) && !o.frost && !o.sensorFault);
  in.room = 21.1f; o = tl_control(st, in, p); CHECK(o.chEnable);   // inside band: stays on
  in.room = 21.3f; o = tl_control(st, in, p); CHECK(!o.chEnable);  // above band: off
  in.room = 20.9f; o = tl_control(st, in, p); CHECK(!o.chEnable);  // inside band: stays off
  in.room = 20.8f; o = tl_control(st, in, p); CHECK(o.chEnable);   // below band: on again

  // clamp and anti-windup: huge error for an hour must not push the integrator past its limit
  st = CtrlState(); in.room = 10.0f; in.target = 25.0f;
  for (int i = 0; i < 3600; i++) o = tl_control(st, in, p);
  CHECK(NEAR(o.flow, 70.0f, 0.001) && st.integ <= p.iMax + 0.001f);
  CHECK(st.integ < 1.0f);                                          // saturated from the start: barely integrated
  // small persistent error: integrator works and is bounded
  st = CtrlState(); in.room = 20.7f; in.target = 21.0f;
  for (int i = 0; i < 36000; i++) o = tl_control(st, in, p);
  CHECK(st.integ > 5.0f && st.integ <= p.iMax + 0.001f && o.flow <= 70.0f);

  // weather compensation
  CHECK(NEAR(tl_weather_base(15), 30, 0.01) && NEAR(tl_weather_base(0), 51, 0.01) && NEAR(tl_weather_base(-10), 65, 0.01));
  CHECK(NEAR(tl_weather_base(30), 30, 0.01) && NEAR(tl_weather_base(-30), 65, 0.01));
  st = CtrlState(); in = { false, true, 20.0f, 21.0f, true, 0.0f, 1.0f };
  o = tl_control(st, in, p); CHECK(NEAR(o.flow, 51 + 12 + 0.01, 0.05));

  // Off: no heat until frost threshold, then frost guard with hysteresis
  st = CtrlState(); in = { true, true, 15.0f, 21.0f, false, 0, 1.0f };
  o = tl_control(st, in, p); CHECK(!o.chEnable && !o.frost);
  in.room = 5.9f; o = tl_control(st, in, p); CHECK(o.chEnable && o.frost && NEAR(o.flow, 40, 0.01));
  in.room = 7.0f; o = tl_control(st, in, p); CHECK(o.chEnable && o.frost);     // still inside band
  in.room = 7.6f; o = tl_control(st, in, p); CHECK(!o.chEnable && !o.frost);
  // leaving Off clears the frost latch
  in.room = 5.0f; o = tl_control(st, in, p); CHECK(o.frost);
  in.modeOff = false; in.target = 5.0f; o = tl_control(st, in, p); CHECK(!o.frost);

  // sensor fault
  st = CtrlState(); in = { false, false, 0, 21.0f, false, 0, 1.0f };
  o = tl_control(st, in, p); CHECK(o.chEnable && o.sensorFault && NEAR(o.flow, 40, 0.01));
  in.modeOff = true; o = tl_control(st, in, p); CHECK(!o.chEnable && !o.sensorFault);

  // --- sensor plausibility ---
  SensorFilter f;
  CHECK(!tl_sensor_accept(f, 85.0f, -10, 50, 1.5f, 0.3f));        // out of range
  CHECK(!tl_sensor_accept(f, NAN, -10, 50, 1.5f, 0.3f));
  CHECK(tl_sensor_accept(f, 21.0f, -10, 50, 1.5f, 0.3f) && NEAR(f.value, 21.0, 0.001));
  CHECK(tl_sensor_accept(f, 21.4f, -10, 50, 1.5f, 0.3f) && NEAR(f.value, 21.12, 0.001));
  CHECK(!tl_sensor_accept(f, 30.0f, -10, 50, 1.5f, 0.3f));        // glitch 1
  CHECK(tl_sensor_accept(f, 21.2f, -10, 50, 1.5f, 0.3f));         // normal again, reject counter reset
  CHECK(!tl_sensor_accept(f, 30.0f, -10, 50, 1.5f, 0.3f));
  CHECK(!tl_sensor_accept(f, 30.0f, -10, 50, 1.5f, 0.3f));
  CHECK(!tl_sensor_accept(f, 30.0f, -10, 50, 1.5f, 0.3f));
  CHECK(tl_sensor_accept(f, 30.0f, -10, 50, 1.5f, 0.3f));         // persistent: believed on the 4th
  CHECK(f.value > 23.0f);

  printf(fails ? "%d FAILED\n" : "all logic tests passed\n", fails);
  return fails ? 1 : 0;
}

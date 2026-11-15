// Real-time clock (PCF85063). Keeps time without Wi-Fi; NTP can correct it later.
#include "clock.h"
#include "state.h"
#include <Arduino.h>
#include <Wire.h>
#include "SensorPCF85063.hpp"

static SensorPCF85063 rtc;
static bool rtcOk = false;

// 0 = Monday .. 6 = Sunday
static uint8_t dow_of(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;   // 0 = Sunday
  return (uint8_t)((w + 6) % 7);
}

void clock_init() {
  rtcOk = rtc.begin(Wire);
  if (!rtcOk) { Serial.println("PCF85063 not found"); return; }
  clock_tick();
  Serial.printf("clock %s %02u:%02u dow=%u\n", st.clockOk ? "ok" : "not set", st.hour, st.minute, st.dow);
}

void clock_tick() {
  if (!rtcOk) { st.clockOk = false; return; }
  RTC_DateTime dt = rtc.getDateTime();
  // a never-set RTC powers up around year 2000; treat anything before 2025 as "not set"
  st.clockOk = dt.getYear() >= 2025 && dt.getHour() < 24 && dt.getMinute() < 60;
  st.hour = dt.getHour();
  st.minute = dt.getMinute();
  if (st.clockOk) st.dow = dow_of(dt.getYear(), dt.getMonth(), dt.getDay());
}

void clock_set(uint8_t dow, uint8_t hour, uint8_t minute) {
  if (!rtcOk) return;
  // No date entry on the device: pick a reference week (Mon 2026-11-02) so the weekday is right.
  rtc.setDateTime(2026, 11, 2 + (dow % 7), hour % 24, minute % 60, 0);
  clock_tick();
}

void clock_set_datetime(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
  if (!rtcOk) return;
  rtc.setDateTime(y, mo, d, h, mi, s);
  clock_tick();
}

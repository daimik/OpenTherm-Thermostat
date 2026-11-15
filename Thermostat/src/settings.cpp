// Persist user-facing settings in NVS so a reboot or power cut does not reset the thermostat.
#include "settings.h"
#include "state.h"
#include <Arduino.h>
#include <Preferences.h>

struct __attribute__((packed)) Saved {
  uint8_t  version;
  uint8_t  mode;
  int16_t  target_c10;
  uint8_t  brightness, volume;
  uint8_t  sounds, reserved2, reserved1;  // no longer used (were "night dim" and "show humidity"); kept so stored settings still load
  int8_t   tempOffset_c10;
  uint8_t  heatRate_c10;     // learned warm-up rate, 0.1 K/h
  uint8_t  sleepScreen;      // newest field of the stored layout; must stay last
};
#define SAVED_V2_SIZE (sizeof(Saved) - 1)

static Saved last;
static uint32_t dirtySince = 0;
static Preferences prefs;

static Saved snapshot() {
  Saved s;
  s.version = 3;
  s.mode = st.mode;
  // while an Auto override is active the dial value is temporary: do not persist it
  s.target_c10 = (st.mode == MODE_AUTO) ? last.target_c10 : (int16_t)lroundf(st.target * 10);
  s.brightness = st.brightness; s.volume = st.volume;
  s.sounds = st.sounds; s.reserved2 = 0; s.reserved1 = 1;
  s.sleepScreen = st.sleepScreen;
  s.tempOffset_c10 = (int8_t)lroundf(st.tempOffset * 10);
  s.heatRate_c10 = (uint8_t)lroundf(constrain(st.heatRate, 0.3f, 3.0f) * 10);
  return s;
}

void settings_load() {
  Saved s = {};
  prefs.begin("thermo", true);
  bool ok = false;
  if (prefs.isKey("cfg")) {
    size_t len = prefs.getBytesLength("cfg");
    if (len == sizeof(s))          ok = prefs.getBytes("cfg", &s, sizeof(s)) == sizeof(s) && s.version == 3;
    else if (len == SAVED_V2_SIZE) { ok = prefs.getBytes("cfg", &s, SAVED_V2_SIZE) == SAVED_V2_SIZE && s.version == 2; s.sleepScreen = 1; }
  }
  prefs.end();
  if (ok) {
    st.mode = (ThermoMode)constrain((int)s.mode, 0, 2);
    st.target = constrain(s.target_c10 / 10.0f, 5.0f, 30.0f);
    st.brightness = constrain((int)s.brightness, 10, 100);
    st.volume = constrain((int)s.volume, 0, 100);
    st.sounds = s.sounds; st.sleepScreen = s.sleepScreen;
    st.tempOffset = s.tempOffset_c10 / 10.0f;
    st.heatRate = constrain(s.heatRate_c10 / 10.0f, 0.3f, 3.0f);
    Serial.println("settings loaded");
  }
  last = {};
  last.target_c10 = (int16_t)lroundf(st.target * 10);
  last = snapshot();
}

void settings_tick() {
  Saved now = snapshot();
  if (memcmp(&now, &last, sizeof(now)) == 0) { dirtySince = 0; return; }
  if (!dirtySince) { dirtySince = millis(); return; }
  if (millis() - dirtySince < 3000) return;
  prefs.begin("thermo", false);
  prefs.putBytes("cfg", &now, sizeof(now));
  prefs.end();
  last = now;
  dirtySince = 0;
  Serial.println("settings saved");
}

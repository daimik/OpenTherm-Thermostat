// Weekly schedule for Auto mode: 4 blocks on weekdays, 4 on the weekend.
// Dial change in Auto = override that lasts until the next block starts.
// Preheat: if the next block is warmer, heating starts early enough to be there on time,
// using the warm-up rate learned in control.cpp.
// The lookup itself is tl_active_block() in thermo_logic.h (unit-tested on the PC).

#include "schedule.h"
#include "state.h"
#include <Arduino.h>
#include <Preferences.h>
#include "evlog.h"

ThermoSchedule sched;
const char *const kBlockNames[SCHED_BLOCKS] = {"Wake", "Away", "Home", "Night"};

static const ThermoSchedule kDefault = {
  { {6 * 60 + 30, 205}, {8 * 60 + 30, 170}, {17 * 60, 210}, {22 * 60 + 30, 165} },
  { {8 * 60,      205}, {10 * 60,     200}, {17 * 60, 210}, {23 * 60,      165} },
};

static Preferences prefs;
static int lastKey = -1;

void schedule_load() {
  sched = kDefault;
  prefs.begin("thermo", true);
  if (prefs.isKey("sched") && prefs.getBytesLength("sched") == sizeof(sched)) prefs.getBytes("sched", &sched, sizeof(sched));
  prefs.end();
}

void schedule_save() {
  prefs.begin("thermo", false);
  prefs.putBytes("sched", &sched, sizeof(sched));
  prefs.end();
  lastKey = -1;                       // re-evaluate with the new blocks
  elog("schedule saved");
}

void schedule_tick() {
  if (st.mode != MODE_AUTO) { st.overrideActive = false; st.autoActive = false; st.preheat = false; lastKey = -1; return; }
  if (!st.clockOk) { st.autoActive = false; st.preheat = false; return; }     // no clock: Auto just holds the dial value

  ActiveBlock a = tl_active_block(sched, st.dow, st.hour * 60 + st.minute);
  st.autoActive = true;
  st.nextChangeMin = a.nextStartMin;

  if (a.key != lastKey) {            // new block: schedule wins again
    lastKey = a.key;
    st.overrideActive = false;
  }
  if (st.overrideActive) { st.preheat = false; return; }

  float target = a.temp_c10 / 10.0f;
  bool pre = false;
  if (a.nextTemp_c10 > a.temp_c10 && st.sensorOk) {
    float next = a.nextTemp_c10 / 10.0f;
    uint16_t need = tl_preheat_minutes(next - st.room, st.heatRate);
    if (need > 0 && a.minutesToNext <= need) { target = next; pre = true; }
  }
  st.preheat = pre;
  st.target = target;
}

void schedule_override() {
  if (st.mode == MODE_AUTO && st.clockOk) st.overrideActive = true;
}

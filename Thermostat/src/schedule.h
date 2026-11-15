#pragma once
#include <stdint.h>
#include "thermo_logic.h"      // SchedBlock, ThermoSchedule, SCHED_BLOCKS

extern ThermoSchedule sched;
extern const char *const kBlockNames[SCHED_BLOCKS];

void schedule_load();          // from flash (NVS), defaults if empty
void schedule_save();
void schedule_tick();          // call ~1 Hz before control_tick(): applies Auto target / handles override
void schedule_override();      // user moved the dial in Auto: hold until next block

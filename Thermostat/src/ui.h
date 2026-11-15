#pragma once
#include <lvgl.h>

void ui_init();      // build all screens, show Home
void ui_update();    // push current ThermoState into widgets (call ~1 s)
void ui_history_tick(); // call ~1 Hz; samples the chart every 5 min
void ui_sleep_enter();  // show the sleep screen (temperature, mode, target)
void ui_sleep_exit();   // back to Home

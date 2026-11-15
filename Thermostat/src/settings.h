#pragma once
void settings_load();   // from flash; call before ui_init()
void settings_tick();   // call ~1 Hz; saves 3 s after the last change

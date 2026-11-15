// Device health: watchdog and crash counter with safe mode.
#pragma once
void health_boot();          // first thing in setup(): reset reason, crash counter, safe mode
void health_start_watchdog();// end of setup()
void health_feed();          // every loop()
void health_tick();          // 1 Hz

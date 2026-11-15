#pragma once
void sensor_init();   // probes for SHT4x (SHT40/41/45) at 0x44 on the shared I2C bus
void sensor_tick();   // call ~1 Hz; measures every 5 s

#pragma once
#include <stdint.h>

void clock_init();                                   // PCF85063 on the shared I2C bus
void clock_tick();                                   // call ~1 Hz: refresh st.hour/minute/dow/clockOk
void clock_set(uint8_t dow, uint8_t hour, uint8_t minute);   // dow: 0=Mon .. 6=Sun
void clock_set_datetime(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s);  // from NTP

// Sensirion SHT4x (SHT40/SHT41/SHT45) room sensor.
//
// Wiring: the sensor gets its own I2C bus (Wire1) on two free header pins, so a loose or shorted
// sensor lead cannot disturb the board's main bus (touch, power chip, clock, audio).
//   default  SDA = GPIO48 (header pin 23), SCL = GPIO47 (header pin 21), 3V3 = pin 31, GND = pin 29
//   change with -DSENSOR_SDA_PIN=.. -DSENSOR_SCL_PIN=.. in platformio.ini
// Avoid GPIO45 and GPIO46 for this: they are boot strapping pins and a sensor breakout pulls its lines high.
// If nothing answers there, the main bus (header pins 28 SDA / 26 SCL) is tried as well.
//
// Tiny driver, no library: one high-precision measurement every 5 s, CRC checked, lightly smoothed.
// Plausibility (tl_sensor_accept in thermo_logic.h, unit-tested): a reading must be inside 0..45 °C;
// a jump of more than 1.5 K from the smoothed value is refused three times, then believed.
// Six unusable readings in a row (30 s), or a raw value frozen for 30 min, mark the sensor as faulty.
// There is no simulated room any more: without a sensor the thermostat shows SENSOR FAULT and the
// control loop falls back to its defined safe behaviour. A missing sensor is looked for again every 30 s,
// so plugging one in does not need a restart.

#include "sensor.h"
#include "state.h"
#include <Arduino.h>
#include <Wire.h>
#include "thermo_logic.h"
#include "evlog.h"

#ifndef SENSOR_SDA_PIN
#define SENSOR_SDA_PIN  48
#endif
#ifndef SENSOR_SCL_PIN
#define SENSOR_SCL_PIN  47
#endif

#define SHT4X_ADDR      0x44
#define SHT4X_MEAS_HP   0xFD      // high precision, ~8.3 ms
#define SHT4X_SOFTRESET 0x94

static bool present = false;
static TwoWire *bus = &Wire;          // the bus the sensor was found on
static uint8_t fails = 0;
static uint32_t lastMs = 0;
static bool first = true;
static uint16_t lastRaw = 0;
static uint16_t sameRawCount = 0;
static SensorFilter filt;
static bool reportedMissing = false;
static uint32_t lastProbeMs = 0;

static uint8_t crc8(const uint8_t *d, int n) {
  uint8_t c = 0xFF;
  for (int i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
  }
  return c;
}

static bool measure(float *t, float *rh, uint16_t *raw) {
  bus->beginTransmission(SHT4X_ADDR);
  bus->write(SHT4X_MEAS_HP);
  if (bus->endTransmission() != 0) return false;
  delay(10);
  if (bus->requestFrom(SHT4X_ADDR, 6) != 6) return false;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = bus->read();
  if (crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5]) return false;
  uint16_t rt = (d[0] << 8) | d[1], rr = (d[3] << 8) | d[4];
  *raw = rt;
  *t = -45.0f + 175.0f * rt / 65535.0f;
  *rh = constrain(-6.0f + 125.0f * rr / 65535.0f, 0.0f, 100.0f);
  return true;
}

static bool probe(TwoWire *w) {
  w->beginTransmission(SHT4X_ADDR);
  return w->endTransmission() == 0;
}

void sensor_init() {
  present = false;
  if (Wire1.begin(SENSOR_SDA_PIN, SENSOR_SCL_PIN, 100000) && probe(&Wire1)) {
    bus = &Wire1; present = true;
    elog("SHT4x found on its own bus (SDA GPIO%d, SCL GPIO%d)", SENSOR_SDA_PIN, SENSOR_SCL_PIN);
  } else if (probe(&Wire)) {
    bus = &Wire; present = true;
    elog("SHT4x found on the main bus (header pins 28/26)");
  } else {
    if (!reportedMissing) elog("SHT4x not found on GPIO%d/GPIO%d or on the main bus", SENSOR_SDA_PIN, SENSOR_SCL_PIN);
    reportedMissing = true;
  }
  if (present) {
    bus->beginTransmission(SHT4X_ADDR); bus->write(SHT4X_SOFTRESET); bus->endTransmission();
    delay(2);
  }
}

void sensor_tick() {
  uint32_t now = millis();
  if (!present) {
    st.sensorOk = false;
    st.sensorFault = true;
    st.sensorPresent = false;
    if (now - lastProbeMs > 30000) { lastProbeMs = now; sensor_init(); }   // was it plugged in meanwhile?
    if (!present) return;
    first = true; fails = 0; filt = SensorFilter();                        // start clean with the new sensor
  }
  st.sensorPresent = true;
  if (now - lastMs < 5000 && !first) return;
  lastMs = now;

  float t, rh; uint16_t raw;
  bool ok = measure(&t, &rh, &raw);
  if (ok) {
    st.roomRaw = t + st.tempOffset;
    sameRawCount = (raw == lastRaw) ? sameRawCount + 1 : 0;
    lastRaw = raw;
    if (sameRawCount > 360) ok = false;                            // bit-identical for 30 min: the sensor is stuck
  }
  if (ok) ok = tl_sensor_accept(filt, t + st.tempOffset, 0.0f, 45.0f, 1.5f, 0.3f);

  if (ok) {
    st.room = filt.value;
    if (first) { st.humidity = rh; first = false; } else st.humidity += (rh - st.humidity) * 0.3f;
    st.sensorOk = true;
    st.sensorFault = false;
    fails = 0;
  } else {
    st.sensorRejects++;
    if (fails < 250) fails++;
    if (fails >= 6) { st.sensorOk = false; st.sensorFault = true; }   // 30 s without a usable reading
  }
}

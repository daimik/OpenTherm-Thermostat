#pragma once
#include <stdint.h>

enum ThermoMode : uint8_t { MODE_AUTO = 0, MODE_MANUAL = 1, MODE_OFF = 2 };

// Single shared state. Written ONLY by the main task. Other tasks (MQTT, BLE callbacks, audio)
// read a snapshot via shared.h or hand data to the main task through queues/flags.
struct ThermoState {
  // room
  float room       = 21.4f;
  float humidity   = 48.0f;
  bool  sensorOk   = false;     // a plausible room temperature is available
  bool  sensorPresent = false;  // an SHT4x answered on the bus
  bool  sensorFault = false;    // no sensor, or it stopped giving plausible values
  float tempOffset = 0.0f;      // added to the sensor reading (self-heating correction)
  float outside    = 0.0f;
  bool  outsideOk  = false;     // a fresh outdoor temperature is available (boiler sensor or HA, max 2 h old)
  uint32_t outsideMs = 0;

  // thermostat
  float target     = 21.5f;
  ThermoMode mode  = MODE_AUTO;
  bool  heating    = false;     // CH demand currently sent to the boiler
  bool  frostActive = false;    // Off mode, but heating to keep the room above freezing
  bool  preheat    = false;     // Auto: heating early for the next block
  float heatRate   = 1.0f;      // learned warm-up speed of the room, K per hour
  float flowRequest = 0.0f;     // flow temperature the control loop is asking for, 0 when idle

  // boiler (from the node over BLE)
  float flow       = 0.0f;
  float ret        = 0.0f;
  float dhw        = 0.0f;
  float pressure   = 0.0f;
  uint8_t modulation = 0;
  bool  flame      = false;
  bool  boilerLinked = false;   // OpenTherm responses arriving at the node
  bool  bleLinked    = false;   // authenticated BLE session with the node
  bool  bleAuthFailed = false;  // last device answering as CV-Boiler did not know the key
  bool  bleVersionMismatch = false;
  bool  boilerFallback = false;
  bool  boilerSimulated = false;
  bool  boilerFault  = false;
  bool  nodeFrost    = false;   // node's own frost protection is active
  bool  nodeClamped  = false;   // node limited the requested flow temperature
  bool  nodeHold     = false;   // node is delaying a heating on/off change (minimum on/off time)
  char  lastFault[12] = "none";
  uint32_t burnerSecondsToday = 0;
  float gasToday   = 0.0f;      // estimate from modulation, m3
  int   bleRssi    = 0;
  uint32_t bleReconnects = 0;
  bool  nodePinned = false;

  // network
  bool wifiOk      = false;
  int  wifiRssi    = 0;
  char ip[16]      = "";
  bool haOk        = false;

  // clock + schedule
  bool    clockOk = false;
  uint8_t hour = 0, minute = 0;
  uint8_t dow = 0;                // 0 = Mon .. 6 = Sun
  bool    autoActive = false;     // Auto is following the schedule
  bool    overrideActive = false; // dial moved in Auto, holds until next block
  uint16_t nextChangeMin = 0;     // minutes since midnight of the next block start

  // display settings
  bool sleepScreen  = true;     // after a minute without touch show only clock, temperature, mode and target
  uint8_t brightness = 70;
  bool    sounds     = true;
  uint8_t volume     = 60;

  // health
  bool  safeMode   = false;     // started after repeated crashes: no Wi-Fi, MQTT or sound
  char  resetReason[16] = "";
  uint8_t crashCount = 0;
  uint32_t uptimeS = 0;

  // diagnostics for the status page (written by the modules that own them, read like everything else)
  bool  ctrlDemand = false;     // control loop: heat demand after hysteresis
  float ctrlBase   = 0.0f;      // flow base from the heating curve (or the fixed base without outdoor value)
  float ctrlInteg  = 0.0f;      // integral part of the flow request, degrees
  float roomRaw    = 0.0f;      // latest unfiltered reading, offset included
  uint32_t sensorRejects = 0;   // readings refused since boot (out of range, sudden jump, bad checksum)
  uint32_t slowFrames = 0;      // screen repaints that took 70 ms or longer, since boot
  uint16_t slowFrameWorstMs = 0;
};

extern ThermoState st;

// implemented in main.cpp
void backlight_set(uint8_t percent);   // sets the level the display should have while awake
void display_wake();                   // counts as a touch: restarts the sleep and dim timers

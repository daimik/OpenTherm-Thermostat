// AXP2101 PMU bring-up, mirrors Waveshare factory bsp_board.cpp so codec, amp,
// sensors and backlight rails are guaranteed on regardless of previous state.
#define XPOWERS_CHIP_AXP2101
#include <Arduino.h>
#include <Wire.h>
#include "XPowersLib.h"
#include "power.h"

static XPowersPMU pmu;

bool power_init() {
  if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, 8, 7)) {
    Serial.println("AXP2101 not found");
    return false;
  }
  pmu.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
  pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);
  pmu.setSysPowerDownVoltage(2600);
  pmu.setDC1Voltage(3300);
  pmu.setDC2Voltage(1000);
  pmu.setDC3Voltage(3300);
  pmu.setDC4Voltage(1000);
  pmu.setDC5Voltage(3300);
  pmu.setALDO1Voltage(3300);
  pmu.setALDO2Voltage(3300);
  pmu.setALDO3Voltage(3300);
  pmu.setALDO4Voltage(3300);
  pmu.setBLDO1Voltage(1500);
  pmu.setBLDO2Voltage(2800);
  pmu.setCPUSLDOVoltage(1000);
  pmu.setDLDO1Voltage(3300);
  pmu.enableDC2(); pmu.enableDC3(); pmu.enableDC4(); pmu.enableDC5();
  pmu.enableALDO1(); pmu.enableALDO2(); pmu.enableALDO3(); pmu.enableALDO4();
  pmu.enableBLDO1(); pmu.enableBLDO2(); pmu.enableCPUSLDO();
  pmu.enableDLDO1(); pmu.enableDLDO2();
  pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
  pmu.setPowerKeyPressOnTime(XPOWERS_POWERON_128MS);
  pmu.disableTSPinMeasure();
  pmu.enableBattDetection();
  pmu.enableVbusVoltageMeasure();
  Serial.println("AXP2101 rails on");
  return true;
}

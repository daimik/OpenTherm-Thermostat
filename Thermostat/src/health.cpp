// Health supervision.
//
// Watchdog: the main loop must come round within WDT_TIMEOUT_S or the chip restarts.
//
// Crash counter: resets caused by a crash, watchdog or brownout are counted in flash. Two minutes of
// normal running clears the count. At three in a row the firmware starts in SAFE MODE: display,
// thermostat logic and the boiler link only, with Wi-Fi, MQTT and sound left off, because those are
// the parts most likely to be the cause and the heating does not need them.

#include "health.h"
#include "state.h"
#include <Arduino.h>
#include <Preferences.h>
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "evlog.h"

#define WDT_TIMEOUT_S      30
#define STABLE_AFTER_S     120
#define SAFE_MODE_CRASHES  3

static Preferences prefs;
static bool stableMarked = false;

static const char *reason_text(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "restart";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:  return "int watchdog";
    case ESP_RST_TASK_WDT: return "watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:               return "other";
  }
}

void health_boot() {
  esp_reset_reason_t r = esp_reset_reason();
  bool abnormal = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
  snprintf(st.resetReason, sizeof(st.resetReason), "%s", reason_text(r));

  prefs.begin("health", false);
  uint8_t crashes = prefs.getUChar("crashes", 0);
  if (abnormal) { if (crashes < 250) crashes++; prefs.putUChar("crashes", crashes); }
  prefs.end();

  st.crashCount = crashes;
  elog("reset reason: %s, crashes in a row: %u", st.resetReason, crashes);

  st.safeMode = crashes >= SAFE_MODE_CRASHES;
  if (st.safeMode) elog("SAFE MODE: Wi-Fi, MQTT and sound are off. Heating and the boiler link run normally.");
}

void health_start_watchdog() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  // core 3.x (ESP-IDF 5): the watchdog takes a configuration struct and may already be running
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

void health_feed() { esp_task_wdt_reset(); }

void health_tick() {
  uint32_t up = millis() / 1000;
  st.uptimeS = up;

  if (!stableMarked && up >= STABLE_AFTER_S) {
    stableMarked = true;
    if (st.crashCount) {
      prefs.begin("health", false); prefs.putUChar("crashes", 0); prefs.end();
      elog("running stable, crash counter cleared");
    }
  }
}

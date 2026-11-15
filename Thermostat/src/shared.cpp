#include "shared.h"
#include "schedule.h"
#include "evlog.h"
#include <Arduino.h>
#include <math.h>

struct Cmd { uint8_t type; float value; };

static SemaphoreHandle_t mux;
static QueueHandle_t q;
static ThermoState snap;
volatile bool g_mqttConnected = false;

void shared_init() {
  mux = xSemaphoreCreateMutex();
  q = xQueueCreate(8, sizeof(Cmd));
}

void shared_publish() {
  if (xSemaphoreTake(mux, pdMS_TO_TICKS(20)) != pdTRUE) return;
  snap = st;
  xSemaphoreGive(mux);
}

void shared_snapshot(ThermoState *out) {
  xSemaphoreTake(mux, portMAX_DELAY);
  *out = snap;
  xSemaphoreGive(mux);
}

bool shared_post(CmdType type, float value) {
  Cmd c = { (uint8_t)type, value };
  return xQueueSend(q, &c, 0) == pdTRUE;
}

void shared_apply_commands() {
  Cmd c;
  while (xQueueReceive(q, &c, 0) == pdTRUE) {
    switch (c.type) {
      case CMD_SET_TARGET:
        if (c.value >= 5 && c.value <= 30) { st.target = roundf(c.value * 2) / 2; schedule_override(); elog("Home Assistant set the target to %.1f", st.target); }
        break;
      case CMD_SET_MODE: {
        int m = (int)c.value;
        if (m >= 0 && m <= 2) { st.mode = (ThermoMode)m; elog("Home Assistant set the mode to %s", m == 0 ? "Auto" : (m == 1 ? "Manual" : "Off")); }
        break;
      }
      case CMD_SET_OUTSIDE:
        if (c.value > -50 && c.value < 60) { st.outside = c.value; st.outsideOk = true; st.outsideMs = millis(); }
        break;
    }
  }
}

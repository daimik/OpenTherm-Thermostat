// Waveshare ESP32-S3-Touch-LCD-3.5 thermostat firmware: hardware bring-up and the main loop.
// Pins and init sequence taken from Waveshare's 11_lvgl_arduino_v8 example.
//
// The main task owns the state (st) and everything LVGL. Per second, in order:
//   commands from other tasks -> sensor -> clock -> schedule -> control -> statistics -> health
//   -> sleep and backlight -> settings save -> chart -> UI -> event log -> snapshots for the MQTT and web tasks

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include "TCA9554.h"
#include "TouchDrvFT6X36.hpp"
#include "state.h"
#include "ui.h"
#include "audio.h"
#include "power.h"
#include "ble_link.h"
#include "control.h"
#include "clock.h"
#include "schedule.h"
#include "net.h"
#include "mqtt.h"
#include "sensor.h"
#include "settings.h"
#include "shared.h"
#include "health.h"
#include "evlog.h"
#include "web.h"

// ---- board pins (ESP32-S3-Touch-LCD-3.5) ----
#define PIN_BL      6
#define PIN_MISO    2
#define PIN_MOSI    1
#define PIN_SCLK    5
#define PIN_LCD_DC  3
#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 7
#define LCD_W       320
#define LCD_H       480
#define EXIO_LCD_RST 1      // TCA9554 pin 1 drives the panel reset
#define EXIO_PA_EN   7      // TCA9554 pin 7: speaker amplifier enable (factory firmware drives it high)
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ   80000000   // Waveshare's own ESP-IDF demos run this panel at 80 MHz; build with -DLCD_SPI_HZ=40000000 if the picture shows noise
#endif

TCA9554 tca(0x20);
Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, -1 /*CS*/, PIN_SCLK, PIN_MOSI, PIN_MISO);
Arduino_GFX *gfx = new Arduino_ST7796(bus, -1 /*RST via TCA*/, 1 /*rotation: landscape*/, true, LCD_W, LCD_H);
TouchDrvFT6X36 touch;

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_color_t *drawBuf;

ThermoState st;

// The LEDC API changed between Arduino core 2.x (channel based) and 3.x (pin based).
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
#define BL_ATTACH()     ledcAttach(PIN_BL, 5000, 8)
#define BL_WRITE(duty)  ledcWrite(PIN_BL, (duty))
#else
#define BL_ATTACH()     do { ledcSetup(0, 5000, 8); ledcAttachPin(PIN_BL, 0); } while (0)
#define BL_WRITE(duty)  ledcWrite(0, (duty))
#endif

// ---- display power: awake or asleep ----
#define SLEEP_AFTER_MS     60000UL  // no touch for this long: sleep screen
#define SLEEP_BRIGHTNESS   25       // percent while asleep, never brighter than the user's own setting

static uint32_t lastTouchMs = 0;
static bool dimmed = false;         // asleep with reduced backlight: the next touch only wakes the display
static bool asleep = false;         // the sleep screen is showing
static bool swallowTouch = false;   // the touch that woke the display is ignored until the finger lifts
static bool wakeNow = false;        // set by the touch driver, handled in loop() so waking does not wait for the 1 s tick
static uint8_t awakeLevel = 70;     // what the user asked for
static uint8_t shownLevel = 255;    // what the backlight is at (255 = not set yet)

static void backlight_apply(uint8_t percent) {
  if (percent > 100) percent = 100;
  if (percent == shownLevel) return;
  shownLevel = percent;
  BL_WRITE((uint32_t)percent * 255 / 100);
}

void backlight_set(uint8_t percent) {
  awakeLevel = percent;
  if (!dimmed) backlight_apply(percent);
}

void display_wake() { lastTouchMs = millis(); }

static void display_tick() {
  uint32_t idle = millis() - lastTouchMs;
  // things a person must see keep the full screen on
  bool stayAwake = st.sensorFault || st.boilerFault || st.safeMode;
  bool wantSleep = st.sleepScreen && !stayAwake && idle > SLEEP_AFTER_MS;

  if (wantSleep != asleep) {
    asleep = wantSleep;
    if (asleep) ui_sleep_enter(); else ui_sleep_exit();
  }
  dimmed = wantSleep;
  uint8_t level = awakeLevel;
  if (wantSleep && level > SLEEP_BRIGHTNESS) level = SLEEP_BRIGHTNESS;
  backlight_apply(level);
}

// Burner time and a rough gas estimate; both reset at midnight.
static void stats_tick() {
  static uint8_t lastDay = 255;
  if (st.clockOk && st.dow != lastDay) { if (lastDay != 255) { st.burnerSecondsToday = 0; st.gasToday = 0; } lastDay = st.dow; }
  if (st.bleLinked && st.boilerLinked && st.flame) {
    st.burnerSecondsToday++;
    // Tzerra Ace 39C: about 3.6 m3/h of gas at 100 % load. Estimate only, no gas meter involved.
    st.gasToday += (st.modulation / 100.0f) * 3.6f / 3600.0f;
  }
}

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif
  lv_disp_flush_ready(drv);
}

// Called by LVGL after every refresh with the time it took. Anything a finger can feel gets logged.
static void perf_cb(lv_disp_drv_t *, uint32_t ms, uint32_t px) {
  if (ms < 70) return;
  st.slowFrames++;                                    // shown on the status page
  if (ms > st.slowFrameWorstMs) st.slowFrameWorstMs = ms > 65535 ? 65535 : (uint16_t)ms;
  Serial.printf("slow frame: %lu ms for %lu px\n", (unsigned long)ms, (unsigned long)px);
}

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  int16_t x[1], y[1];
  if (touch.getPoint(x, y, 1)) {
    lastTouchMs = millis();
    if (dimmed) { swallowTouch = true; wakeNow = true; }        // wake first; this touch presses nothing
    if (swallowTouch) { data->state = LV_INDEV_STATE_REL; return; }
    data->state = LV_INDEV_STATE_PR;
    // panel is portrait 320x480; LVGL runs landscape 480x320 (rotation 1)
    data->point.x = y[0];
    data->point.y = LCD_W - 1 - x[0];
  } else {
    swallowTouch = false;
    data->state = LV_INDEV_STATE_REL;
  }
}

static void lcd_reset() {
  tca.write1(EXIO_LCD_RST, 1); delay(10);
  tca.write1(EXIO_LCD_RST, 0); delay(10);
  tca.write1(EXIO_LCD_RST, 1); delay(200);
}

static void tick_cb(lv_timer_t *) {
  static uint8_t n = 0;
  shared_apply_commands();          // target / mode / outdoor temperature posted by the MQTT task
  st.haOk = g_mqttConnected;
  sensor_tick();
  clock_tick();
  schedule_tick();
  control_tick();
  stats_tick();
  health_tick();
  display_tick();
  settings_tick();
  ui_history_tick();
  ui_update();
  evlog_state_tick(st);             // one log line for every part of the state that changed this second
  shared_publish();                 // consistent copy of st for the MQTT task
  web_publish();                    // and for the status page (does nothing when the page is not built in)
  if (++n % 5 == 0)
    Serial.printf("hb %lus ble=%d wifi=%d mqtt=%d room=%.1f tgt=%.1f req=%.1f flow=%.1f heat=%d%s%s%s%s heap=%u min=%u\n",
                  (unsigned long)(millis() / 1000), st.bleLinked, st.wifiOk, st.haOk, st.room, st.target, st.flowRequest, st.flow, st.heating,
                  st.frostActive ? " FROST" : "", st.preheat ? " PREHEAT" : "", st.sensorFault ? " SENSOR-FAULT" : "", st.safeMode ? " SAFE-MODE" : "",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(15);        // a log line may wait 15 ms at most; without this, no monitor on USB means 250 ms stalls
  // Give a serial monitor a moment to attach, but do not hold up the boot: on the wall there is no
  // USB host and this would otherwise cost four seconds of black screen at every start.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1200) delay(10);
  evlog_init();                     // before the first elog(): the ring for the status page
  elog("thermostat display boot");
  shared_init();
  health_boot();                    // reset reason, crash counter, safe mode

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);            // every device on this bus is fine at 400 kHz; touch reads take a quarter of the time
  power_init();
  delay(50);
  tca.begin();
  tca.pinMode1(EXIO_LCD_RST, OUTPUT);
  tca.pinMode1(EXIO_PA_EN, OUTPUT);
  tca.write1(EXIO_PA_EN, 1);
  lcd_reset();

  if (!touch.begin(Wire, FT6X36_SLAVE_ADDRESS)) {
    elog("FT6336 touch controller not found");
  }
  if (!gfx->begin(LCD_SPI_HZ)) {
    elog("gfx->begin() failed");
  }
  gfx->fillScreen(RGB565_BLACK);
  BL_ATTACH();
  backlight_set(st.brightness);

  lv_init();
  uint32_t w = gfx->width(), h = gfx->height();   // 480 x 320 after rotation
  // One 40-line buffer in internal RAM (38 KB). The flush is blocking, so a second buffer buys nothing
  // and internal RAM is what Wi-Fi + BLE need.
  uint32_t bufSize = w * 40;
  drawBuf = (lv_color_t *)heap_caps_malloc(bufSize * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&draw_buf, drawBuf, NULL, bufSize);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = w;
  disp_drv.ver_res = h;
  disp_drv.flush_cb = disp_flush;
  disp_drv.draw_buf = &draw_buf;
  disp_drv.monitor_cb = perf_cb;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  clock_init();
  sensor_init();
  schedule_load();
  settings_load();
  backlight_set(st.brightness);
  ui_init();
  if (!st.safeMode) {
    audio_init();
    net_init();        // Wi-Fi first: scan + WPA handshake want the radio to themselves
    mqtt_init();
    web_init();        // read-only status page; only built in when secrets.h defines WEB_PASS
  }
  // BLE is started from loop() once Wi-Fi is up, or after 20 s without Wi-Fi (at once in safe mode)
  lv_timer_create(tick_cb, 1000, NULL);
  audio_play(SND_CONFIRM);
  shared_publish();
  health_start_watchdog();          // from here on the loop must come round or the chip restarts
  Serial.printf("setup done, PSRAM %u KB\n", ESP.getPsramSize() / 1024);
}

void loop() {
  health_feed();
  if (!st.safeMode) {
    net_loop();
  }
  lv_timer_handler();
  if (wakeNow) { wakeNow = false; display_tick(); }   // a touch on a sleeping display wakes it at once
  static bool bleStarted = false;
  if (!bleStarted && (st.safeMode || st.wifiOk || millis() > 20000)) {
    bleStarted = true;
    elog("%s", st.wifiOk ? "starting BLE (Wi-Fi is up)" : "starting BLE (Wi-Fi not up after 20 s)");
    ble_link_init();
  }
  if (bleStarted) ble_link_loop();
  delay(2);
}

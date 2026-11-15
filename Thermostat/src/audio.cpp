// Synthesised UI sounds through the ES8311 codec + onboard speaker.
// Legacy I2S driver (Arduino core 2.0.x / IDF 4.4). No files, no SD card.

#include "audio.h"
#include "state.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "driver/i2s.h"
#include "es8311.h"

#define I2S_PORT     I2S_NUM_0
#define PIN_MCLK     12
#define PIN_BCLK     13
#define PIN_LRCLK    15
#define PIN_DOUT     16
#define PIN_DIN      14
#define SAMPLE_RATE  22050
#define MCLK_MULT    256

static es8311_handle_t codec = nullptr;
static QueueHandle_t q;
static bool ready = false;

struct Tone { float f0; float f1; uint16_t ms; float decay; };

// id -> one or two short segments
static const Tone kTones[5][2] = {
  { {1800, 1800, 18, 220.0f}, {0, 0, 0, 0} },            // TICK: short soft click
  { {1200, 1600, 45, 40.0f},  {0, 0, 0, 0} },            // UP: rising blip
  { {1600, 1200, 45, 40.0f},  {0, 0, 0, 0} },            // DOWN: falling blip
  { {1047, 1047, 70, 25.0f},  {1568, 1568, 110, 18.0f} },// CONFIRM: C6 -> G6
  { {330, 330, 120, 10.0f},   {0, 0, 0, 0} },            // ERROR: low buzz
};

static void synth_segment(const Tone &t, int16_t *buf, size_t &n, float gain) {
  size_t samples = (size_t)SAMPLE_RATE * t.ms / 1000;
  float phase = 0;
  for (size_t i = 0; i < samples; i++) {
    float tt = (float)i / SAMPLE_RATE;
    float f = t.f0 + (t.f1 - t.f0) * (float)i / samples;
    phase += 2.0f * (float)M_PI * f / SAMPLE_RATE;
    float env = expf(-t.decay * tt);
    // 2 ms attack to avoid the pop
    if (tt < 0.002f) env *= tt / 0.002f;
    int16_t v = (int16_t)(sinf(phase) * env * gain * 32000.0f);
    buf[n++] = v;   // L
    buf[n++] = v;   // R
  }
}

static void player_task(void *) {
  const size_t cap = (SAMPLE_RATE / 1000) * 200 * 2;    // up to 200 ms stereo
  int16_t *buf = (int16_t *)heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);   // PSRAM, not internal RAM
  if (!buf) buf = (int16_t *)malloc(cap * sizeof(int16_t));
  uint8_t id;
  for (;;) {
    if (xQueueReceive(q, &id, portMAX_DELAY) != pdTRUE) continue;
    if (!ready || id > SND_ERROR) continue;
    size_t n = 0;
    float gain = 0.6f;
    synth_segment(kTones[id][0], buf, n, gain);
    if (kTones[id][1].ms) synth_segment(kTones[id][1], buf, n, gain);
    // 10 ms of silence so the amp does not click on the tail
    for (int i = 0; i < SAMPLE_RATE / 100 * 2; i++) buf[n++] = 0;
    size_t written = 0;
    i2s_write(I2S_PORT, buf, n * sizeof(int16_t), &written, portMAX_DELAY);
  }
}

void audio_init() {
  codec = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  if (!codec) { Serial.println("es8311 create failed"); return; }
  es8311_clock_config_t clk = {};
  clk.mclk_inverted = false;
  clk.sclk_inverted = false;
  clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency = SAMPLE_RATE * MCLK_MULT;
  clk.sample_frequency = SAMPLE_RATE;
  bool ok = false;
  for (int i = 0; i < 3 && !ok; i++) {
    ok = es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) == ESP_OK;
    if (!ok) { Serial.printf("es8311 init failed (try %d)\n", i + 1); delay(100); }
  }
  if (!ok) { return; }
  es8311_microphone_config(codec, false);
  audio_set_volume(st.volume);

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 256;
  cfg.use_apll = true;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = SAMPLE_RATE * MCLK_MULT;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) { Serial.println("i2s install failed"); return; }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = PIN_MCLK;
  pins.bck_io_num = PIN_BCLK;
  pins.ws_io_num = PIN_LRCLK;
  pins.data_out_num = PIN_DOUT;
  pins.data_in_num = PIN_DIN;
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);

  q = xQueueCreate(4, sizeof(uint8_t));
  xTaskCreatePinnedToCore(player_task, "audio", 4096, NULL, 2, NULL, 0);
  ready = true;
  Serial.println("audio ready");
}

void audio_play(SoundId id) {
  if (!ready || !st.sounds) return;
  uint8_t v = id;
  xQueueSend(q, &v, 0);
}

void audio_set_volume(uint8_t pct) {
  if (!codec) return;
  if (pct > 100) pct = 100;
  es8311_voice_volume_set(codec, pct, NULL);
}

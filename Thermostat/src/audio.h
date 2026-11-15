#pragma once
#include <stdint.h>

enum SoundId : uint8_t { SND_TICK = 0, SND_UP, SND_DOWN, SND_CONFIRM, SND_ERROR };

void audio_init();                 // ES8311 + I2S, starts player task
void audio_play(SoundId id);       // non-blocking, drops if busy
void audio_set_volume(uint8_t pct);// 0..100

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define VOICE_SAMPLE_RATE 16000

typedef void (*audio_progress_cb_t)(uint32_t duration_ms, uint8_t level, void *context);
typedef void (*audio_complete_cb_t)(
    uint8_t *pcm, size_t pcm_bytes, esp_err_t result, void *context);

typedef struct {
    audio_progress_cb_t on_progress;
    audio_complete_cb_t on_complete;
    void *context;
} audio_recorder_callbacks_t;

esp_err_t audio_recorder_init(void);
esp_err_t audio_recorder_start(const audio_recorder_callbacks_t *callbacks);
esp_err_t audio_recorder_pause(void);
void audio_recorder_stop(void);
bool audio_recorder_is_recording(void);

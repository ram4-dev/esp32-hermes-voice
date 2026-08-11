#include "audio_recorder.h"

#include <limits.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define READ_FRAMES 512
#define INPUT_CHANNELS 2
#define INPUT_SAMPLES (READ_FRAMES * INPUT_CHANNELS)
#define INPUT_BYTES (INPUT_SAMPLES * sizeof(int16_t))
#define OUTPUT_BYTES (READ_FRAMES * sizeof(int16_t))

static const char *TAG = "audio_recorder";
static esp_codec_dev_handle_t s_microphone;
static volatile bool s_recording;
static TaskHandle_t s_task;
static audio_recorder_callbacks_t s_callbacks;

static uint8_t audio_level(const int16_t *samples, size_t count)
{
    int32_t peak = 0;
    for (size_t index = 0; index < count; ++index) {
        int32_t value = samples[index];
        if (value < 0) {
            value = -value;
        }
        if (value > peak) {
            peak = value;
        }
    }
    int32_t percent = (peak * 100) / INT16_MAX;
    return (uint8_t)(percent > 100 ? 100 : percent);
}

static void recorder_task(void *argument)
{
    (void)argument;
    const size_t capacity =
        (size_t)VOICE_SAMPLE_RATE * sizeof(int16_t) * CONFIG_VOICE_MAX_RECORDING_SECONDS;
    uint8_t *pcm = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        s_recording = false;
        s_task = NULL;
        s_callbacks.on_complete(NULL, 0, ESP_ERR_NO_MEM, s_callbacks.context);
        vTaskDelete(NULL);
        return;
    }

    size_t used = 0;
    int16_t input[INPUT_SAMPLES];
    int64_t started_at = esp_timer_get_time();
    int64_t last_progress = started_at;
    esp_err_t result = ESP_OK;
    while (s_recording && used + OUTPUT_BYTES <= capacity) {
        int read_result = esp_codec_dev_read(s_microphone, input, INPUT_BYTES);
        if (read_result != ESP_OK) {
            ESP_LOGE(TAG, "microphone read failed: %d", read_result);
            result = ESP_FAIL;
            break;
        }
        int16_t *mono = (int16_t *)(pcm + used);
        for (size_t frame = 0; frame < READ_FRAMES; ++frame) {
            int32_t mixed = (int32_t)input[frame * 2] + input[frame * 2 + 1];
            mono[frame] = (int16_t)(mixed / 2);
        }
        uint8_t level = audio_level(mono, READ_FRAMES);
        used += OUTPUT_BYTES;
        int64_t now = esp_timer_get_time();
        if (s_callbacks.on_progress != NULL && now - last_progress >= 100000) {
            s_callbacks.on_progress((uint32_t)((now - started_at) / 1000), level,
                                    s_callbacks.context);
            last_progress = now;
        }
    }

    s_recording = false;
    s_task = NULL;
    if (result != ESP_OK || used == 0) {
        free(pcm);
        pcm = NULL;
        if (result == ESP_OK) {
            result = ESP_ERR_INVALID_SIZE;
        }
    }
    s_callbacks.on_complete(pcm, used, result, s_callbacks.context);
    vTaskDelete(NULL);
}

esp_err_t audio_recorder_init(void)
{
    s_microphone = bsp_audio_codec_microphone_init();
    if (s_microphone == NULL) {
        return ESP_FAIL;
    }
    esp_codec_dev_sample_info_t format = {
        .sample_rate = VOICE_SAMPLE_RATE,
        .channel = INPUT_CHANNELS,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_microphone, &format) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_gain(s_microphone, 30.0f);
    return ESP_OK;
}

esp_err_t audio_recorder_start(const audio_recorder_callbacks_t *callbacks)
{
    if (callbacks == NULL || callbacks->on_complete == NULL || s_microphone == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_recording || s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_callbacks = *callbacks;
    s_recording = true;
    if (xTaskCreatePinnedToCore(recorder_task, "voice_record", 6144, NULL, 6, &s_task, 1) !=
        pdPASS) {
        s_recording = false;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_recorder_stop(void)
{
    s_recording = false;
}

bool audio_recorder_is_recording(void)
{
    return s_recording;
}

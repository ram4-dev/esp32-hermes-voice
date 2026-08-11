#include "audio_recorder.h"

#include "sdkconfig.h"
#include <limits.h>
#include <stdlib.h>

#include "audio_player.h"
#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define READ_FRAMES 512
#define INPUT_CHANNELS 2
#define INPUT_SAMPLES (READ_FRAMES * INPUT_CHANNELS)
#define INPUT_BYTES (INPUT_SAMPLES * sizeof(int16_t))
#define OUTPUT_BYTES (READ_FRAMES * sizeof(int16_t))

static const char *TAG = "audio_recorder";
static esp_codec_dev_handle_t s_microphone;
static SemaphoreHandle_t s_codec_lock;
static volatile bool s_recording;
static bool s_codec_open;
static bool s_completing;
static TaskHandle_t s_task;
static audio_recorder_callbacks_t s_callbacks;

static esp_err_t open_microphone_locked(void)
{
    if (s_microphone == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_codec_open) {
        return ESP_OK;
    }
    esp_codec_dev_sample_info_t format = {
        .sample_rate = VOICE_SAMPLE_RATE,
        .channel = INPUT_CHANNELS,
        .bits_per_sample = 16,
    };
    int result = esp_codec_dev_open(s_microphone, &format);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "microphone open failed: status=%d (%s)", result,
                 esp_err_to_name((esp_err_t)result));
        return (esp_err_t)result;
    }
    int gain_result = esp_codec_dev_set_in_gain(s_microphone, 30.0f);
    if (gain_result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "microphone gain setup failed: status=%d (%s)", gain_result,
                 esp_err_to_name((esp_err_t)gain_result));
    }
    s_codec_open = true;
    return ESP_OK;
}

static esp_err_t close_microphone_locked(void)
{
    if (!s_codec_open) {
        return ESP_OK;
    }
    int result = esp_codec_dev_close(s_microphone);
    if (result == ESP_CODEC_DEV_OK) {
        s_codec_open = false;
    }
    return (esp_err_t)result;
}

static bool microphone_is_open(void)
{
    bool is_open = false;
    if (s_codec_lock != NULL) {
        xSemaphoreTake(s_codec_lock, portMAX_DELAY);
        is_open = s_codec_open;
        xSemaphoreGive(s_codec_lock);
    }
    return is_open;
}

static esp_err_t prepare_completion(void)
{
    xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    s_recording = false;
    s_completing = true;
    esp_err_t result = close_microphone_locked();
    s_task = NULL;
    xSemaphoreGive(s_codec_lock);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "microphone close failed: status=%d (%s)", result,
                 esp_err_to_name(result));
    }
    return result;
}

static void finish_completion(void)
{
    xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    s_completing = false;
    xSemaphoreGive(s_codec_lock);
}

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
        esp_err_t close_result = prepare_completion();
        esp_err_t result = close_result == ESP_OK ? ESP_ERR_NO_MEM : close_result;
        ESP_LOGI(TAG, "complete: status=%d (%s), pcm_bytes=0", result,
                 esp_err_to_name(result));
        s_callbacks.on_complete(NULL, 0, result, s_callbacks.context);
        finish_completion();
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
        if (read_result != INPUT_BYTES) {
            ESP_LOGE(TAG, "microphone read failed: bytes=%d/%d", read_result, INPUT_BYTES);
            result = read_result < 0 ? (esp_err_t)read_result : ESP_FAIL;
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

    esp_err_t close_result = prepare_completion();
    if (close_result != ESP_OK) {
        free(pcm);
        pcm = NULL;
        used = 0;
        result = close_result;
    }
    if (result != ESP_OK || used == 0) {
        free(pcm);
        pcm = NULL;
        if (result == ESP_OK) {
            result = ESP_ERR_INVALID_SIZE;
        }
    }
    ESP_LOGI(TAG, "complete: status=%d (%s), pcm_bytes=%u", result,
             esp_err_to_name(result), (unsigned)used);
    s_callbacks.on_complete(pcm, used, result, s_callbacks.context);
    finish_completion();
    vTaskDelete(NULL);
}

esp_err_t audio_recorder_init(void)
{
    s_codec_lock = xSemaphoreCreateMutex();
    if (s_codec_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_microphone = bsp_audio_codec_microphone_init();
    if (s_microphone == NULL) {
        vSemaphoreDelete(s_codec_lock);
        s_codec_lock = NULL;
        return ESP_FAIL;
    }
    s_codec_open = false;
    return ESP_OK;
}

esp_err_t audio_recorder_start(const audio_recorder_callbacks_t *callbacks)
{
    if (callbacks == NULL || callbacks->on_complete == NULL || s_microphone == NULL ||
        s_codec_lock == NULL) {
        ESP_LOGE(TAG, "start rejected: status=%d (%s)", ESP_ERR_INVALID_ARG,
                 esp_err_to_name(ESP_ERR_INVALID_ARG));
        return ESP_ERR_INVALID_ARG;
    }
    for (int attempt = 0; attempt < 100 && !microphone_is_open(); ++attempt) {
        if (!audio_player_is_playing()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    if (s_recording || s_task != NULL || s_completing || audio_player_is_playing()) {
        xSemaphoreGive(s_codec_lock);
        ESP_LOGE(TAG, "start rejected: status=%d (%s)", ESP_ERR_INVALID_STATE,
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t open_result = open_microphone_locked();
    if (open_result != ESP_OK) {
        xSemaphoreGive(s_codec_lock);
        ESP_LOGE(TAG, "start microphone open failed: status=%d (%s)", open_result,
                 esp_err_to_name(open_result));
        return open_result;
    }
    s_callbacks = *callbacks;
    s_recording = true;
    if (xTaskCreatePinnedToCore(recorder_task, "voice_record", 6144, NULL, 6, &s_task, 1) !=
        pdPASS) {
        s_recording = false;
        s_task = NULL;
        esp_err_t close_result = close_microphone_locked();
        xSemaphoreGive(s_codec_lock);
        if (close_result != ESP_OK) {
            ESP_LOGE(TAG, "start cleanup close failed: status=%d (%s)", close_result,
                     esp_err_to_name(close_result));
        }
        ESP_LOGE(TAG, "start failed: status=%d (%s)", ESP_ERR_NO_MEM,
                 esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_codec_lock);
    ESP_LOGI(TAG, "start: status=%d (%s)", ESP_OK, esp_err_to_name(ESP_OK));
    return ESP_OK;
}

esp_err_t audio_recorder_pause(void)
{
    if (s_codec_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_codec_lock, portMAX_DELAY);
    if (s_recording || s_task != NULL) {
        xSemaphoreGive(s_codec_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    if (s_codec_open) {
        result = (esp_err_t)esp_codec_dev_close(s_microphone);
        if (result == ESP_OK) {
            s_codec_open = false;
        }
    }
    xSemaphoreGive(s_codec_lock);
    ESP_LOGI(TAG, "pause: status=%d (%s)", result, esp_err_to_name(result));
    return result;
}

void audio_recorder_stop(void)
{
    s_recording = false;
    ESP_LOGI(TAG, "stop: status=%d (%s)", ESP_OK, esp_err_to_name(ESP_OK));
}

bool audio_recorder_is_recording(void)
{
    return s_recording;
}

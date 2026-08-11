#include "audio_player.h"

#include <string.h>

#include "audio_recorder.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "audio_player";
static const char *NVS_NAMESPACE = "voice";
static const char *NVS_VOLUME_KEY = "volume";
static const char *NVS_AUDIO_ENABLED_KEY = "audio_enabled";
static esp_codec_dev_handle_t s_speaker;
static SemaphoreHandle_t s_lock;
static volatile bool s_playing;
static volatile bool s_abort;
static bool s_codec_open;
static uint8_t s_volume = AUDIO_PLAYER_DEFAULT_VOLUME;
static bool s_audio_enabled = true;

static esp_err_t load_volume(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "volume NVS open failed: %s; using default", esp_err_to_name(result));
        return ESP_OK;
    }
    uint8_t volume = AUDIO_PLAYER_DEFAULT_VOLUME;
    uint8_t audio_enabled = 1;
    result = nvs_get_u8(handle, NVS_VOLUME_KEY, &volume);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "volume NVS read failed: %s; using default", esp_err_to_name(result));
    }
    esp_err_t audio_result = nvs_get_u8(handle, NVS_AUDIO_ENABLED_KEY, &audio_enabled);
    if (audio_result != ESP_OK && audio_result != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "audio setting NVS read failed: %s; using default", esp_err_to_name(audio_result));
    }
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = nvs_set_u8(handle, NVS_VOLUME_KEY, volume);
    }
    if (audio_result == ESP_ERR_NVS_NOT_FOUND && result == ESP_OK) {
        result = nvs_set_u8(handle, NVS_AUDIO_ENABLED_KEY, audio_enabled);
    }
    if (result == ESP_OK && audio_result == ESP_OK) {
        result = nvs_commit(handle);
    } else if (result == ESP_OK && audio_result == ESP_ERR_NVS_NOT_FOUND) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    s_volume = volume <= 100 ? volume : AUDIO_PLAYER_DEFAULT_VOLUME;
    s_audio_enabled = audio_enabled != 0;
    return ESP_OK;
}

static esp_err_t save_volume(uint8_t volume)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_u8(handle, NVS_VOLUME_KEY, volume);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

esp_err_t audio_player_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(load_volume());
    s_speaker = bsp_audio_codec_speaker_init();
    if (s_speaker == NULL) {
        ESP_LOGE(TAG, "speaker codec initialization failed");
        return ESP_FAIL;
    }
    esp_err_t result = (esp_err_t)esp_codec_dev_set_out_vol(s_speaker, s_volume);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "could not set speaker volume: %s", esp_err_to_name(result));
        return result;
    }
    ESP_LOGI(TAG, "speaker volume loaded: %u", (unsigned)s_volume);
    return ESP_OK;
}

esp_err_t audio_player_set_volume(uint8_t volume)
{
    if (volume > 100 || s_lock == NULL || s_speaker == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t result = (esp_err_t)esp_codec_dev_set_out_vol(s_speaker, volume);
    if (result == ESP_OK) {
        result = save_volume(volume);
        if (result == ESP_OK) {
            s_volume = volume;
        }
    }
    xSemaphoreGive(s_lock);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "volume update failed: %s", esp_err_to_name(result));
    }
    return result;
}

uint8_t audio_player_get_volume(void)
{
    return s_volume;
}

static esp_err_t save_audio_enabled(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_u8(handle, NVS_AUDIO_ENABLED_KEY, enabled ? 1 : 0);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

esp_err_t audio_player_set_audio_enabled(bool enabled)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t result = save_audio_enabled(enabled);
    if (result == ESP_OK) {
        s_audio_enabled = enabled;
    }
    xSemaphoreGive(s_lock);
    return result;
}

bool audio_player_get_audio_enabled(void)
{
    return s_audio_enabled;
}

esp_err_t audio_player_stream_begin(void)
{
    if (s_speaker == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_playing || s_abort || !s_audio_enabled) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_playing = true;
    xSemaphoreGive(s_lock);

    esp_err_t result = audio_recorder_pause();
    if (result != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_playing = false;
        xSemaphoreGive(s_lock);
        return result;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_abort) {
        s_playing = false;
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_codec_dev_sample_info_t format = {
        .sample_rate = AUDIO_PLAYER_SAMPLE_RATE,
        .channel = AUDIO_PLAYER_CHANNELS,
        .bits_per_sample = AUDIO_PLAYER_BITS_PER_SAMPLE,
    };
    result = (esp_err_t)esp_codec_dev_open(s_speaker, &format);
    if (result == ESP_OK) {
        s_codec_open = true;
    } else {
        s_playing = false;
    }
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t audio_player_stream_write(uint8_t *data, size_t length)
{
    if (data == NULL || length == 0 || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_abort || !s_codec_open || !s_playing) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = (esp_err_t)esp_codec_dev_write(s_speaker, data, (int)length);
    xSemaphoreGive(s_lock);
    return result;
}

esp_err_t audio_player_stream_finish(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    if (s_codec_open) {
        result = (esp_err_t)esp_codec_dev_close(s_speaker);
        if (result == ESP_OK) {
            s_codec_open = false;
        }
    }
    s_playing = false;
    s_abort = false;
    xSemaphoreGive(s_lock);
    esp_err_t recorder_result = audio_recorder_pause();
    return result == ESP_OK ? recorder_result : result;
}

void audio_player_stop(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_abort = s_playing || s_codec_open;
    s_playing = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "playback cancellation requested");
}

bool audio_player_is_playing(void)
{
    return s_playing;
}

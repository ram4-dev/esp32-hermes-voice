#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_player.h"
#include "audio_recorder.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "voice_client.h"
#include "voice_ui.h"
#include "wav.h"
#include "wifi_manager.h"

static const char *TAG = "voice_app";
static SemaphoreHandle_t s_pending_lock;
static uint8_t *s_pending_wav;
static size_t s_pending_wav_bytes;
static char s_pending_request_id[40];
static char s_cancelled_request_id[40];
static voice_client_result_t s_last_result;
static volatile bool s_uploading;

static void make_request_id(char *target, size_t capacity)
{
    uint32_t random_values[4] = {
        esp_random(), esp_random(), esp_random(), esp_random()
    };
    snprintf(target, capacity, "%08lx%08lx%08lx%08lx",
             (unsigned long)random_values[0], (unsigned long)random_values[1],
             (unsigned long)random_values[2], (unsigned long)random_values[3]);
}

static bool request_is_current(const char *request_id)
{
    bool current;
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    current = request_id != NULL && strcmp(s_pending_request_id, request_id) == 0;
    xSemaphoreGive(s_pending_lock);
    return current;
}

static bool request_was_cancelled(const char *request_id)
{
    bool cancelled;
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    cancelled = request_id != NULL && strcmp(s_cancelled_request_id, request_id) == 0;
    xSemaphoreGive(s_pending_lock);
    return cancelled;
}

static void clear_pending(bool clear_request_id)
{
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    free(s_pending_wav);
    s_pending_wav = NULL;
    s_pending_wav_bytes = 0;
    if (clear_request_id) {
        s_pending_request_id[0] = '\0';
    }
    xSemaphoreGive(s_pending_lock);
}

static void release_uploaded_wav(const char *request_id)
{
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    if (request_id != NULL && strcmp(s_pending_request_id, request_id) == 0) {
        free(s_pending_wav);
        s_pending_wav = NULL;
        s_pending_wav_bytes = 0;
    }
    xSemaphoreGive(s_pending_lock);
}

static void remote_status_changed(voice_remote_status_t status, void *context)
{
    (void)context;
    switch (status) {
    case VOICE_REMOTE_TRANSCRIBING:
        voice_ui_show_working("Transcribiendo", "Convirtiendo tu voz a texto");
        break;
    case VOICE_REMOTE_ASKING_HERMES:
        voice_ui_show_working("Pensando", "Hermes está trabajando");
        break;
    case VOICE_REMOTE_SYNTHESIZING:
        voice_ui_show_working("Sintetizando", "Preparando la voz de Hermes");
        break;
    case VOICE_REMOTE_PLAYING:
        voice_ui_show_playing("Audio de Hermes • presioná para interrumpir");
        break;
    case VOICE_REMOTE_QUEUED:
    default:
        voice_ui_show_working("En cola", "Proxmox recibió el audio");
        break;
    }
}

static void finish_upload_task(const char *request_id)
{
    if (request_is_current(request_id)) {
        s_uploading = false;
    }
}

static void upload_task(void *argument)
{
    (void)argument;
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    uint8_t *wav = s_pending_wav;
    size_t wav_bytes = s_pending_wav_bytes;
    char request_id[sizeof(s_pending_request_id)];
    strlcpy(request_id, s_pending_request_id, sizeof(request_id));
    xSemaphoreGive(s_pending_lock);

    if (!wifi_manager_wait_connected(15000)) {
        if (request_is_current(request_id)) {
            voice_ui_show_error("Sin conexión. El audio sigue listo para reintentar.", true);
            finish_upload_task(request_id);
        }
        vTaskDelete(NULL);
        return;
    }

    voice_ui_show_working("Enviando", "Subiendo el WAV a Proxmox");
    voice_client_result_t result;
    int err = voice_client_run(wav, wav_bytes, request_id, remote_status_changed,
                               NULL, &result);
    if (err != ESP_OK) {
        if (request_is_current(request_id)) {
            voice_ui_show_error(result.error[0] ? result.error : "Error al procesar el audio", true);
            finish_upload_task(request_id);
        }
        vTaskDelete(NULL);
        return;
    }

    if (!request_is_current(request_id) || request_was_cancelled(request_id)) {
        finish_upload_task(request_id);
        vTaskDelete(NULL);
        return;
    }
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    s_last_result = result;
    xSemaphoreGive(s_pending_lock);
    release_uploaded_wav(request_id);
    voice_ui_show_response(result.transcript, result.answer, result.truncated);

    char audio_error[256];
    err = voice_client_play_speech(request_id, remote_status_changed, NULL,
                                   audio_error, sizeof(audio_error));
    if (request_was_cancelled(request_id)) {
        finish_upload_task(request_id);
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK) {
        xSemaphoreTake(s_pending_lock, portMAX_DELAY);
        strlcpy(s_last_result.audio_error, audio_error, sizeof(s_last_result.audio_error));
        xSemaphoreGive(s_pending_lock);
        voice_ui_show_audio_error(audio_error);
    } else {
        voice_ui_show_response(result.transcript, result.answer, result.truncated);
    }
    finish_upload_task(request_id);
    vTaskDelete(NULL);
}

static void audio_retry_task(void *argument)
{
    (void)argument;
    char request_id[sizeof(s_pending_request_id)];
    voice_client_result_t result;
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    strlcpy(request_id, s_pending_request_id, sizeof(request_id));
    result = s_last_result;
    xSemaphoreGive(s_pending_lock);

    char audio_error[256];
    int err = voice_client_play_speech(request_id, remote_status_changed, NULL,
                                       audio_error, sizeof(audio_error));
    if (!request_is_current(request_id) || request_was_cancelled(request_id)) {
        finish_upload_task(request_id);
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK) {
        voice_ui_show_audio_error(audio_error);
    } else {
        voice_ui_show_response(result.transcript, result.answer, result.truncated);
    }
    finish_upload_task(request_id);
    vTaskDelete(NULL);
}

static void start_upload(void)
{
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    bool has_audio = s_pending_wav != NULL && s_pending_wav_bytes > WAV_HEADER_SIZE;
    xSemaphoreGive(s_pending_lock);
    if (!has_audio || s_uploading) {
        return;
    }
    s_uploading = true;
    if (xTaskCreatePinnedToCore(upload_task, "voice_upload", 12288, NULL, 5, NULL, 0) !=
        pdPASS) {
        s_uploading = false;
        voice_ui_show_error("No hay memoria para iniciar el envío", true);
    }
}

static void start_audio_retry(void)
{
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    bool has_request = s_pending_request_id[0] != '\0' && s_pending_wav == NULL;
    xSemaphoreGive(s_pending_lock);
    if (!has_request || s_uploading) {
        return;
    }
    s_uploading = true;
    if (xTaskCreatePinnedToCore(audio_retry_task, "audio_retry", 12288, NULL, 5, NULL, 0) !=
        pdPASS) {
        s_uploading = false;
        voice_ui_show_audio_error("No hay memoria para reintentar el audio");
    }
}

static void recording_progress(uint32_t duration_ms, uint8_t level, void *context)
{
    (void)context;
    voice_ui_show_recording(duration_ms, level);
}

static void recording_complete(
    uint8_t *pcm, size_t pcm_bytes, esp_err_t result, void *context)
{
    (void)context;
    if (result != ESP_OK) {
        voice_ui_show_error("No se pudo capturar audio", false);
        return;
    }
    uint8_t *wav = NULL;
    size_t wav_bytes = 0;
    result = wav_create_pcm16_mono(pcm, pcm_bytes, VOICE_SAMPLE_RATE, &wav, &wav_bytes);
    free(pcm);
    if (result != ESP_OK) {
        voice_ui_show_error("No hay memoria para preparar el WAV", false);
        return;
    }

    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    free(s_pending_wav);
    s_pending_wav = wav;
    s_pending_wav_bytes = wav_bytes;
    make_request_id(s_pending_request_id, sizeof(s_pending_request_id));
    xSemaphoreGive(s_pending_lock);
    start_upload();
}

static void record_pressed(void *context)
{
    (void)context;
    bool was_playing = audio_player_is_playing();
    if (was_playing) {
        xSemaphoreTake(s_pending_lock, portMAX_DELAY);
        strlcpy(s_cancelled_request_id, s_pending_request_id,
                sizeof(s_cancelled_request_id));
        xSemaphoreGive(s_pending_lock);
        audio_player_stop();
        /* The playback task will only finish cleanup; the new recording owns the UI. */
        s_uploading = false;
    }
    if (s_uploading && !was_playing) {
        return;
    }
    clear_pending(true);

    audio_recorder_callbacks_t callbacks = {
        .on_progress = recording_progress,
        .on_complete = recording_complete,
        .context = NULL,
    };
    esp_err_t result = audio_recorder_start(&callbacks);
    if (result != ESP_OK) {
        voice_ui_show_error("No se pudo iniciar el micrófono", false);
    } else {
        voice_ui_show_recording(0, 0);
    }
}

static void record_released(void *context)
{
    (void)context;
    audio_recorder_stop();
}

static void retry_pressed(void *context)
{
    (void)context;
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    bool has_wav = s_pending_wav != NULL;
    xSemaphoreGive(s_pending_lock);
    if (has_wav) {
        start_upload();
    } else {
        start_audio_retry();
    }
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    s_pending_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_pending_lock == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    ESP_ERROR_CHECK(audio_player_init());
    voice_ui_callbacks_t ui_callbacks = {
        .on_record_pressed = record_pressed,
        .on_record_released = record_released,
        .on_retry_pressed = retry_pressed,
        .context = NULL,
    };
    ESP_ERROR_CHECK(voice_ui_init(&ui_callbacks));
    ESP_ERROR_CHECK(audio_recorder_init());

    if (strcmp(CONFIG_VOICE_DEVICE_TOKEN, "replace-me") == 0) {
        ESP_LOGW(TAG, "configure a real device token before deployment");
    }
    result = wifi_manager_start();
    if (result != ESP_OK) {
        voice_ui_show_error("Configurá el Wi-Fi con idf.py menuconfig", false);
        ESP_LOGE(TAG, "Wi-Fi startup failed: %s", esp_err_to_name(result));
        return;
    }
    voice_ui_show_idle(wifi_manager_wait_connected(15000));
}

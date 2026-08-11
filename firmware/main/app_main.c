#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_recorder.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
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
static bool s_uploading;

static void make_request_id(char *target, size_t capacity)
{
    uint32_t random_values[4] = {
        esp_random(), esp_random(), esp_random(), esp_random()
    };
    snprintf(target, capacity, "%08lx%08lx%08lx%08lx",
             (unsigned long)random_values[0], (unsigned long)random_values[1],
             (unsigned long)random_values[2], (unsigned long)random_values[3]);
}

static void remote_status_changed(voice_remote_status_t status, void *context)
{
    (void)context;
    if (status == VOICE_REMOTE_TRANSCRIBING) {
        voice_ui_show_working("Transcribiendo", "Convirtiendo tu voz a texto");
    } else if (status == VOICE_REMOTE_ASKING_HERMES) {
        voice_ui_show_working("Pensando", "Hermes está trabajando");
    } else {
        voice_ui_show_working("En cola", "Proxmox recibió el audio");
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
        voice_ui_show_error("Sin conexión. El audio sigue listo para reintentar.", true);
        s_uploading = false;
        vTaskDelete(NULL);
        return;
    }

    voice_ui_show_working("Enviando", "Subiendo el WAV a Proxmox");
    voice_client_result_t result;
    esp_err_t err = voice_client_run(wav, wav_bytes, request_id, remote_status_changed,
                                     NULL, &result);
    if (err == ESP_OK) {
        voice_ui_show_response(result.transcript, result.answer, result.truncated);
        xSemaphoreTake(s_pending_lock, portMAX_DELAY);
        free(s_pending_wav);
        s_pending_wav = NULL;
        s_pending_wav_bytes = 0;
        s_pending_request_id[0] = '\0';
        xSemaphoreGive(s_pending_lock);
    } else {
        voice_ui_show_error(result.error[0] ? result.error : "Error al procesar el audio", true);
    }
    s_uploading = false;
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
    if (xTaskCreatePinnedToCore(upload_task, "voice_upload", 10240, NULL, 5, NULL, 0) !=
        pdPASS) {
        s_uploading = false;
        voice_ui_show_error("No hay memoria para iniciar el envío", true);
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
    if (s_uploading || audio_recorder_is_recording()) {
        return;
    }
    xSemaphoreTake(s_pending_lock, portMAX_DELAY);
    free(s_pending_wav);
    s_pending_wav = NULL;
    s_pending_wav_bytes = 0;
    s_pending_request_id[0] = '\0';
    xSemaphoreGive(s_pending_lock);

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
    start_upload();
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

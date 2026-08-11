#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_player.h"
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
static SemaphoreHandle_t s_state_lock;
static uint8_t *s_pending_wav;
static size_t s_pending_wav_bytes;
static char s_pending_request_id[40];
static voice_client_result_t s_last_result;
static bool s_audio_enabled = true;
static volatile bool s_uploading;

typedef struct operation_token {
    volatile bool cancelled;
    bool recording;
    char request_id[sizeof(s_pending_request_id)];
    uint8_t *wav;
    size_t wav_bytes;
} operation_token_t;

static operation_token_t *s_record_operation;
static operation_token_t *s_upload_operation;

static void make_request_id(char *target, size_t capacity)
{
    uint32_t random_values[4] = {esp_random(), esp_random(), esp_random(), esp_random()};
    snprintf(target, capacity, "%08lx%08lx%08lx%08lx",
             (unsigned long)random_values[0], (unsigned long)random_values[1],
             (unsigned long)random_values[2], (unsigned long)random_values[3]);
}

static bool token_is_current(operation_token_t *token)
{
    bool current;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    current = token != NULL &&
              ((token->recording && s_record_operation == token) ||
               (!token->recording && s_upload_operation == token));
    xSemaphoreGive(s_state_lock);
    return current;
}

static bool token_cancelled(void *context)
{
    operation_token_t *token = context;
    return token == NULL || token->cancelled || !token_is_current(token);
}

static void clear_pending(void)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    free(s_pending_wav);
    s_pending_wav = NULL;
    s_pending_wav_bytes = 0;
    s_pending_request_id[0] = '\0';
    xSemaphoreGive(s_state_lock);
}

static void remote_status_changed(voice_remote_status_t status, void *context)
{
    operation_token_t *token = context;
    if (!token_is_current(token) || token_cancelled(token)) {
        return;
    }
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
        voice_ui_show_playing("Audio de Hermes • presioná CANCELAR para detener");
        break;
    case VOICE_REMOTE_QUEUED:
    default:
        voice_ui_show_working("En cola", "Proxmox recibió el audio");
        break;
    }
}

static void finish_upload(operation_token_t *token, bool keep_wav)
{
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s_upload_operation == token) {
        if (keep_wav && token->wav != NULL && s_pending_wav == NULL) {
            s_pending_wav = token->wav;
            s_pending_wav_bytes = token->wav_bytes;
            token->wav = NULL;
            strlcpy(s_pending_request_id, token->request_id, sizeof(s_pending_request_id));
        }
        s_upload_operation = NULL;
        s_uploading = false;
    }
    xSemaphoreGive(s_state_lock);
    free(token->wav);
    free(token);
}

static void upload_task(void *argument)
{
    operation_token_t *token = argument;
    voice_client_result_t *result = heap_caps_calloc(1, sizeof(*result),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result == NULL) {
            if (token_is_current(token) && !token_cancelled(token)) {
                voice_ui_show_error("No hay memoria para procesar el audio", true);
            }
            finish_upload(token, true);

        vTaskDelete(NULL);
        return;
    }
    if (!wifi_manager_wait_connected(15000)) {
            if (token_is_current(token) && !token_cancelled(token)) {
                voice_ui_show_error("Sin conexión. El audio sigue listo para reintentar.", true);
            }
            free(result);
            finish_upload(token, true);

        vTaskDelete(NULL);
        return;
    }
    voice_ui_show_working("Enviando", "Subiendo el WAV a Proxmox");
    int err = voice_client_run(token->wav, token->wav_bytes, token->request_id,
                               remote_status_changed, token, token_cancelled, token, result);
        if (err != ESP_OK || token_cancelled(token)) {
            bool cancelled = token_cancelled(token);
            if (!cancelled && token_is_current(token)) {
                voice_ui_show_error(result->error[0] ? result->error : "Error al procesar el audio", true);
            }
            free(result);
            finish_upload(token, !cancelled);
            vTaskDelete(NULL);
            return;
        }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_last_result = *result;
    xSemaphoreGive(s_state_lock);
    voice_ui_show_response(result->transcript, result->answer, result->truncated, s_audio_enabled);

    if (s_audio_enabled && !token_cancelled(token)) {
        char audio_error[256];
        err = voice_client_play_speech(token->request_id, remote_status_changed, token,
                                       token_cancelled, token, audio_error, sizeof(audio_error));
            if (token_cancelled(token)) {
                free(result);
                finish_upload(token, false);
                vTaskDelete(NULL);
                return;
            }

        if (err != ESP_OK && s_audio_enabled) {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            strlcpy(s_last_result.audio_error, audio_error, sizeof(s_last_result.audio_error));
            xSemaphoreGive(s_state_lock);
            voice_ui_show_audio_error(audio_error);
        }
    }
    free(result);
    finish_upload(token, false);
    vTaskDelete(NULL);
}

static void audio_retry_task(void *argument)
{
    operation_token_t *token = argument;
    char audio_error[256];
    voice_client_result_t result;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    result = s_last_result;
    xSemaphoreGive(s_state_lock);
    int err = voice_client_play_speech(token->request_id, remote_status_changed, token,
                                       token_cancelled, token, audio_error, sizeof(audio_error));
    if (!token_cancelled(token)) {
        if (!s_audio_enabled) {
            voice_ui_show_response(result.transcript, result.answer, result.truncated, false);
        } else if (err != ESP_OK) {
            voice_ui_show_audio_error(audio_error);
        } else {
            voice_ui_show_response(result.transcript, result.answer, result.truncated, true);
        }
    }
    finish_upload(token, false);
    vTaskDelete(NULL);
}

static void start_upload(void)
{
    operation_token_t *token = calloc(1, sizeof(*token));
    if (token == NULL) {
        voice_ui_show_error("No hay memoria para iniciar el envío", true);
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s_uploading || s_pending_wav == NULL || s_pending_wav_bytes <= WAV_HEADER_SIZE) {
        xSemaphoreGive(s_state_lock);
        free(token);
        return;
    }
    token->recording = false;
    token->wav = s_pending_wav;
    token->wav_bytes = s_pending_wav_bytes;
    strlcpy(token->request_id, s_pending_request_id, sizeof(token->request_id));
    s_pending_wav = NULL;
    s_pending_wav_bytes = 0;
    s_upload_operation = token;
    s_uploading = true;
    xSemaphoreGive(s_state_lock);
    if (xTaskCreatePinnedToCore(upload_task, "voice_upload", 12288, token, 5, NULL, 0) != pdPASS) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_pending_wav = token->wav;
        s_pending_wav_bytes = token->wav_bytes;
        s_upload_operation = NULL;
        s_uploading = false;
        xSemaphoreGive(s_state_lock);
        free(token);
        voice_ui_show_error("No hay memoria para iniciar el envío", true);
    }
}

static void start_audio_retry(void)
{
    if (!s_audio_enabled) {
        return;
    }
    operation_token_t *token = calloc(1, sizeof(*token));
    if (token == NULL) {
        voice_ui_show_audio_error("No hay memoria para reintentar el audio");
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    bool available = !s_uploading && s_pending_request_id[0] != '\0' && s_pending_wav == NULL;
    if (available) {
        token->recording = false;
        strlcpy(token->request_id, s_pending_request_id, sizeof(token->request_id));
        s_upload_operation = token;
        s_uploading = true;
    }
    xSemaphoreGive(s_state_lock);
    if (!available) {
        free(token);
        return;
    }
    if (xTaskCreatePinnedToCore(audio_retry_task, "audio_retry", 12288, token, 5, NULL, 0) !=
        pdPASS) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_upload_operation = NULL;
        s_uploading = false;
        xSemaphoreGive(s_state_lock);
        free(token);
        voice_ui_show_audio_error("No hay memoria para reintentar el audio");
    }
}

static void recording_progress(uint32_t duration_ms, uint8_t level, void *context)
{
    operation_token_t *token = context;
    if (!token_cancelled(token)) {
        voice_ui_show_recording(duration_ms, level);
    }
}

static void recording_complete(uint8_t *pcm, size_t pcm_bytes, esp_err_t result, void *context)
{
    operation_token_t *token = context;
    bool current = token_is_current(token);
    if (!current || token_cancelled(token) || result != ESP_OK) {
        free(pcm);
        if (current) {
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            s_record_operation = NULL;
            xSemaphoreGive(s_state_lock);
            voice_ui_show_idle(wifi_manager_is_connected());
        }
        free(token);
        return;
    }
    uint8_t *wav = NULL;
    size_t wav_bytes = 0;
    result = wav_create_pcm16_mono(pcm, pcm_bytes, VOICE_SAMPLE_RATE, &wav, &wav_bytes);
    free(pcm);
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_record_operation = NULL;
    if (result == ESP_OK) {
        free(s_pending_wav);
        s_pending_wav = wav;
        s_pending_wav_bytes = wav_bytes;
        make_request_id(s_pending_request_id, sizeof(s_pending_request_id));
    }
    xSemaphoreGive(s_state_lock);
    free(token);
    if (result != ESP_OK) {
        voice_ui_show_error("No hay memoria para preparar el WAV", false);
        return;
    }
    start_upload();
}

static void record_pressed(void *context)
{
    (void)context;
    if (audio_player_is_playing()) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        if (s_upload_operation != NULL) {
            s_upload_operation->cancelled = true;
            s_uploading = false;
        }
        xSemaphoreGive(s_state_lock);
        audio_player_stop();
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    bool busy = s_record_operation != NULL || s_uploading;
    xSemaphoreGive(s_state_lock);
    if (busy) {
        return;
    }
    clear_pending();
    operation_token_t *token = calloc(1, sizeof(*token));
    if (token == NULL) {
        voice_ui_show_error("No hay memoria para grabar", false);
        return;
    }
    token->recording = true;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_record_operation = token;
    xSemaphoreGive(s_state_lock);
    audio_recorder_callbacks_t callbacks = {
        .on_progress = recording_progress,
        .on_complete = recording_complete,
        .context = token,
    };
    esp_err_t result = audio_recorder_start(&callbacks);
    if (result != ESP_OK) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_record_operation = NULL;
        xSemaphoreGive(s_state_lock);
        free(token);
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

static void cancel_pressed(void *context)
{
    (void)context;
    operation_token_t *token = NULL;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (s_record_operation != NULL) {
        token = s_record_operation;
    } else if (s_upload_operation != NULL) {
        token = s_upload_operation;
        s_uploading = false;
    }
    if (token != NULL) {
        token->cancelled = true;
    }
    xSemaphoreGive(s_state_lock);
    if (token == NULL) {
        return;
    }
    if (token->recording) {
        audio_recorder_stop();
    } else {
        audio_player_stop();
    }
    voice_ui_show_idle(wifi_manager_is_connected());
}

static void retry_pressed(void *context)
{
    (void)context;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    bool has_wav = s_pending_wav != NULL;
    xSemaphoreGive(s_state_lock);
    if (has_wav) {
        start_upload();
    } else {
        start_audio_retry();
    }
}

static void volume_pressed(void *context)
{
    (void)context;
    uint8_t volume = audio_player_get_volume();
    volume = volume >= 100 ? 0 : (uint8_t)(volume + 10);
    if (audio_player_set_volume(volume) == ESP_OK) {
        voice_ui_show_volume(volume);
    }
}

static void audio_toggle_pressed(void *context)
{
    (void)context;
    bool enabled = !s_audio_enabled;
    if (audio_player_set_audio_enabled(enabled) != ESP_OK) {
        return;
    }
    s_audio_enabled = enabled;
    if (!s_audio_enabled) {
        audio_player_stop();
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        voice_client_result_t result = s_last_result;
        bool has_text = result.answer[0] != '\0';
        xSemaphoreGive(s_state_lock);
        if (has_text) {
            voice_ui_show_response(result.transcript, result.answer, result.truncated, false);
        }
    }
    voice_ui_set_audio_enabled(s_audio_enabled);
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    s_state_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_state_lock == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    ESP_ERROR_CHECK(audio_player_init());
    s_audio_enabled = audio_player_get_audio_enabled();
    voice_ui_callbacks_t ui_callbacks = {
        .on_record_pressed = record_pressed,
        .on_record_released = record_released,
        .on_retry_pressed = retry_pressed,
        .on_cancel_pressed = cancel_pressed,
        .on_volume_pressed = volume_pressed,
        .on_audio_toggle_pressed = audio_toggle_pressed,
        .context = NULL,
    };
    ESP_ERROR_CHECK(voice_ui_init(&ui_callbacks));
    ESP_ERROR_CHECK(audio_recorder_init());

    if (strcmp(CONFIG_VOICE_DEVICE_TOKEN, "replace-me") == 0) {
        ESP_LOGW(TAG, "configure a real device token in ignored local sdkconfig");
    }
    result = wifi_manager_start();
    if (result != ESP_OK) {
        voice_ui_show_error("Configurá el Wi-Fi en sdkconfig local", false);
        ESP_LOGE(TAG, "Wi-Fi startup failed: %s", esp_err_to_name(result));
        return;
    }
    voice_ui_set_audio_enabled(s_audio_enabled);
    voice_ui_show_volume(audio_player_get_volume());
    voice_ui_show_idle(wifi_manager_wait_connected(15000));
}

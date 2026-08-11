#include "voice_client.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RESPONSE_CAPACITY 8192
#define URL_CAPACITY 512

static const char *TAG = "voice_client";

#if CONFIG_VOICE_BRIDGE_EMBED_LOCAL_CA
extern const uint8_t server_root_ca_start[]
    asm("_binary_certs_server_root_ca_pem_start");
#endif

typedef struct {
    char bytes[RESPONSE_CAPACITY];
    size_t length;
    bool overflow;
} response_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    response_buffer_t *response = (response_buffer_t *)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0 && response != NULL) {
        size_t available = RESPONSE_CAPACITY - 1 - response->length;
        size_t to_copy = (size_t)event->data_len;
        if (to_copy > available) {
            to_copy = available;
            response->overflow = true;
        }
        memcpy(response->bytes + response->length, event->data, to_copy);
        response->length += to_copy;
        response->bytes[response->length] = '\0';
    }
    return ESP_OK;
}

static void configure_tls(esp_http_client_config_t *config)
{
#if CONFIG_VOICE_BRIDGE_EMBED_LOCAL_CA
    config->cert_pem = (const char *)server_root_ca_start;
#else
    config->crt_bundle_attach = esp_crt_bundle_attach;
#endif
}

static void set_auth_headers(esp_http_client_handle_t client, const char *request_id)
{
    char authorization[320];
    snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_VOICE_DEVICE_TOKEN);
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "X-Device-Id", CONFIG_VOICE_DEVICE_ID);
    if (request_id != NULL) {
        esp_http_client_set_header(client, "X-Request-Id", request_id);
    }
}

static esp_err_t perform_post(
    const uint8_t *wav, size_t wav_bytes, const char *request_id, int *status_out)
{
    char url[URL_CAPACITY];
    snprintf(url, sizeof(url), "%s/v1/voice/jobs", CONFIG_VOICE_BRIDGE_URL);
    response_buffer_t response = {0};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &response,
        .timeout_ms = 60000,
        .buffer_size = 2048,
        .buffer_size_tx = 4096,
    };
    configure_tls(&config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    set_auth_headers(client, request_id);
    esp_http_client_set_post_field(client, (const char *)wav, (int)wav_bytes);
    esp_err_t result = esp_http_client_perform(client);
    *status_out = esp_http_client_get_status_code(client);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "upload failed: %s", esp_err_to_name(result));
    } else if (response.overflow) {
        result = ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_cleanup(client);
    return result;
}

static esp_err_t perform_get(
    const char *request_id, response_buffer_t *response, int *status_out)
{
    char url[URL_CAPACITY];
    snprintf(url, sizeof(url), "%s/v1/voice/jobs/%s", CONFIG_VOICE_BRIDGE_URL,
             request_id);
    memset(response, 0, sizeof(*response));
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = response,
        .timeout_ms = 30000,
        .buffer_size = 2048,
    };
    configure_tls(&config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    set_auth_headers(client, NULL);
    esp_err_t result = esp_http_client_perform(client);
    *status_out = esp_http_client_get_status_code(client);
    if (response->overflow) {
        result = ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_cleanup(client);
    return result;
}

static void copy_json_string(cJSON *root, const char *name, char *target, size_t capacity)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(value) && value->valuestring != NULL) {
        strlcpy(target, value->valuestring, capacity);
    }
}

esp_err_t voice_client_run(
    const uint8_t *wav,
    size_t wav_bytes,
    const char *request_id,
    voice_status_cb_t status_callback,
    void *status_context,
    voice_client_result_t *result)
{
    if (wav == NULL || wav_bytes == 0 || request_id == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    int http_status = 0;
    esp_err_t err = perform_post(wav, wav_bytes, request_id, &http_status);
    if (err != ESP_OK) {
        strlcpy(result->error, "No se pudo subir el audio", sizeof(result->error));
        return err;
    }
    if (http_status != 200 && http_status != 202) {
        snprintf(result->error, sizeof(result->error), "El servidor rechazó el audio (%d)",
                 http_status);
        return ESP_FAIL;
    }
    if (status_callback != NULL) {
        status_callback(VOICE_REMOTE_QUEUED, status_context);
    }

    const int poll_count = CONFIG_VOICE_POLL_TIMEOUT_SECONDS;
    voice_remote_status_t last_remote_status = VOICE_REMOTE_QUEUED;
    for (int poll = 0; poll < poll_count; ++poll) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        response_buffer_t response;
        err = perform_get(request_id, &response, &http_status);
        if (err != ESP_OK || http_status != 200) {
            continue;
        }
        cJSON *root = cJSON_Parse(response.bytes);
        if (root == NULL) {
            continue;
        }
        cJSON *status_value = cJSON_GetObjectItemCaseSensitive(root, "status");
        const char *remote_status = cJSON_IsString(status_value) ? status_value->valuestring : "";
        if (strcmp(remote_status, "transcribing") == 0) {
            last_remote_status = VOICE_REMOTE_TRANSCRIBING;
        } else if (strcmp(remote_status, "asking_hermes") == 0) {
            last_remote_status = VOICE_REMOTE_ASKING_HERMES;
        } else if (strcmp(remote_status, "completed") == 0) {
            copy_json_string(root, "transcript", result->transcript,
                             sizeof(result->transcript));
            copy_json_string(root, "answer", result->answer, sizeof(result->answer));
            cJSON *truncated = cJSON_GetObjectItemCaseSensitive(root, "truncated");
            result->truncated = cJSON_IsTrue(truncated);
            cJSON_Delete(root);
            return result->answer[0] != '\0' ? ESP_OK : ESP_FAIL;
        } else if (strcmp(remote_status, "failed") == 0) {
            copy_json_string(root, "error", result->error, sizeof(result->error));
            cJSON_Delete(root);
            if (result->error[0] == '\0') {
                strlcpy(result->error, "El procesamiento falló", sizeof(result->error));
            }
            return ESP_FAIL;
        }
        cJSON_Delete(root);
        if (status_callback != NULL) {
            status_callback(last_remote_status, status_context);
        }
    }
    strlcpy(result->error, "Hermes tardó demasiado en responder", sizeof(result->error));
    return ESP_ERR_TIMEOUT;
}

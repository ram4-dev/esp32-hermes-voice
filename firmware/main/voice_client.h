#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    VOICE_REMOTE_QUEUED,
    VOICE_REMOTE_TRANSCRIBING,
    VOICE_REMOTE_ASKING_HERMES,
} voice_remote_status_t;

typedef void (*voice_status_cb_t)(voice_remote_status_t status, void *context);

typedef struct {
    char transcript[2048];
    char answer[4096];
    char error[256];
    bool truncated;
} voice_client_result_t;

esp_err_t voice_client_run(
    const uint8_t *wav,
    size_t wav_bytes,
    const char *request_id,
    voice_status_cb_t status_callback,
    void *status_context,
    voice_client_result_t *result);

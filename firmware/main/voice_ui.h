#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    void (*on_record_pressed)(void *context);
    void (*on_record_released)(void *context);
    void (*on_retry_pressed)(void *context);
    void *context;
} voice_ui_callbacks_t;

esp_err_t voice_ui_init(const voice_ui_callbacks_t *callbacks);
void voice_ui_show_idle(bool connected);
void voice_ui_show_recording(uint32_t duration_ms, uint8_t level);
void voice_ui_show_working(const char *title, const char *detail);
void voice_ui_show_response(const char *transcript, const char *answer, bool truncated);
void voice_ui_show_error(const char *message, bool can_retry);

#ifndef VOICE_UI_H
#define VOICE_UI_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void (*on_record_pressed)(void *context);
    void (*on_record_released)(void *context);
    void (*on_retry_pressed)(void *context);
    void (*on_cancel_pressed)(void *context);
    void (*on_volume_pressed)(void *context);
    void (*on_audio_toggle_pressed)(void *context);
    void *context;
} voice_ui_callbacks_t;

int voice_ui_init(const voice_ui_callbacks_t *callbacks);
void voice_ui_show_idle(bool connected);
void voice_ui_show_recording(uint32_t duration_ms, uint8_t level);
void voice_ui_show_working(const char *title, const char *detail);
void voice_ui_show_playing(const char *detail);
void voice_ui_show_response(const char *transcript, const char *answer, bool truncated,
                            bool audio_enabled);
void voice_ui_show_audio_error(const char *message);
void voice_ui_show_error(const char *message, bool can_retry);
void voice_ui_show_volume(uint8_t volume);
void voice_ui_set_audio_enabled(bool enabled);
void voice_ui_set_tailscale_state(const char *state, bool connected);

#endif /* VOICE_UI_H */

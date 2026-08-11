#include "voice_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

LV_FONT_DECLARE(voice_font_20);

static const char *TAG = "voice_ui";
static const char *SPANISH_GLYPHS = "áéíóúüñÁÉÍÓÚÜÑ¿¡";
static voice_ui_callbacks_t s_callbacks;
static lv_obj_t *s_status;
static lv_obj_t *s_detail;
static lv_obj_t *s_record_button;
static lv_obj_t *s_record_label;
static lv_obj_t *s_new_record_button;
static lv_obj_t *s_new_record_label;
static lv_obj_t *s_retry_button;
static lv_obj_t *s_cancel_button;
static lv_obj_t *s_volume_down_button;
static lv_obj_t *s_volume_up_button;
static lv_obj_t *s_volume_label;
static lv_obj_t *s_audio_button;
static lv_obj_t *s_audio_label;
static lv_obj_t *s_tailscale_label;
static lv_obj_t *s_answer_panel;
static lv_obj_t *s_answer;
static lv_obj_t *s_transcript;
static bool s_recording_from_new_button;
static bool s_record_gesture_active;
static bool s_record_release_requested;
static int64_t s_record_pressed_at_us;
static lv_timer_t *s_record_release_timer;
static bool s_ui_idle;
static bool s_audio_enabled = true;
static uint8_t s_volume;

#define SAFE_X 24
#define SAFE_BOTTOM 24
#define SAFE_W (BSP_LCD_H_RES - (2 * SAFE_X))
#define TOP_BUTTON_Y 24
#define TOP_BUTTON_H 42
#define VOLUME_BUTTON_W 76
#define VOLUME_GAP 8
#define VOLUME_VALUE_X (SAFE_X + (2 * VOLUME_BUTTON_W) + (2 * VOLUME_GAP))
#define VOLUME_VALUE_W 68
#define AUDIO_BUTTON_W 112
#define AUDIO_BUTTON_X (BSP_LCD_H_RES - SAFE_X - AUDIO_BUTTON_W)
#define TAILSCALE_Y 72
#define TOP_Y 98
#define DETAIL_Y 132
#define RECORD_Y_OFFSET 32
#define ACTION_H 50
#define ACTION_Y (BSP_LCD_V_RES - SAFE_BOTTOM - ACTION_H)
#define PANEL_Y 184
#define PANEL_H (ACTION_Y - PANEL_Y - 8)
#define RECORD_RELEASE_GRACE_MS 180

static void set_interactive(bool enabled, bool show_retry, bool show_cancel);
static void show_answer_panel(bool visible);

static void cancel_record_release_timer(void)
{
    if (s_record_release_timer != NULL) {
        lv_timer_del(s_record_release_timer);
        s_record_release_timer = NULL;
    }
}

static void finish_record_release(void)
{
    s_record_release_timer = NULL;
    if (!s_record_gesture_active || !s_record_release_requested) return;
    s_record_gesture_active = false;
    s_record_release_requested = false;
    if (s_callbacks.on_record_released != NULL) {
        s_callbacks.on_record_released(s_callbacks.context);
    }
    s_recording_from_new_button = false;
}

static void record_release_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    finish_record_release();
}

static void show_record_gesture_feedback(void)
{
    s_ui_idle = false;
    if (s_recording_from_new_button) {
        lv_obj_add_flag(s_answer_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_record_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_new_record_button, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_new_record_label, "SOLTAR PARA ENVIAR");
    } else {
        show_answer_panel(false);
    }
    lv_label_set_text(s_status, "Grabando");
    lv_label_set_text(s_detail, "Preparando micrófono…");
    lv_label_set_text(s_record_label, "SOLTAR\nPARA ENVIAR");
    set_interactive(true, false, true);
}

static void schedule_record_release(void)
{
    cancel_record_release_timer();
    s_record_release_requested = true;
    int64_t elapsed_us = esp_timer_get_time() - s_record_pressed_at_us;
    uint32_t elapsed_ms = elapsed_us > 0 ? (uint32_t)(elapsed_us / 1000) : 0;
    uint32_t delay_ms = elapsed_ms < RECORD_RELEASE_GRACE_MS
                            ? RECORD_RELEASE_GRACE_MS - elapsed_ms : 1;
    s_record_release_timer = lv_timer_create(record_release_timer_cb, delay_ms, NULL);
    if (s_record_release_timer == NULL) {
        /* A failed timer allocation must not leave the recorder running. */
        finish_record_release();
    } else {
        lv_timer_set_repeat_count(s_record_release_timer, 1);
    }
}

static void record_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        cancel_record_release_timer();
        s_record_release_requested = false;
        if (s_record_gesture_active) return;
        s_record_gesture_active = true;
        s_record_pressed_at_us = esp_timer_get_time();
        s_recording_from_new_button = lv_event_get_target(event) == s_new_record_button;
        /* Update the visible state before the deferred recorder start. */
        show_record_gesture_feedback();
        if (s_callbacks.on_record_pressed != NULL) {
            s_callbacks.on_record_pressed(s_callbacks.context);
        }
    } else if (code == LV_EVENT_PRESSING) {
        /* Touch jitter can emit PRESS_LOST followed by PRESSING again. */
        if (s_record_gesture_active && s_record_release_requested) {
            s_record_release_requested = false;
            cancel_record_release_timer();
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) &&
               s_record_gesture_active) {
        /* Do not stop on a transient PRESS_LOST or a same-sample release. */
        schedule_record_release();
    }
}

static void retry_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        s_callbacks.on_retry_pressed != NULL) {
        s_callbacks.on_retry_pressed(s_callbacks.context);
    }
}

static void cancel_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        s_callbacks.on_cancel_pressed != NULL) {
        s_callbacks.on_cancel_pressed(s_callbacks.context);
    }
}

static void volume_down_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        s_callbacks.on_volume_down_pressed != NULL) {
        s_callbacks.on_volume_down_pressed(s_callbacks.context);
    }
}

static void volume_up_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        s_callbacks.on_volume_up_pressed != NULL) {
        s_callbacks.on_volume_up_pressed(s_callbacks.context);
    }
}

static void audio_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        s_callbacks.on_audio_toggle_pressed != NULL) {
        s_callbacks.on_audio_toggle_pressed(s_callbacks.context);
    }
}

static void set_interactive(bool enabled, bool show_retry, bool show_cancel)
{
    if (enabled) {
        lv_obj_remove_state(s_record_button, LV_STATE_DISABLED);
        lv_obj_remove_state(s_new_record_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
        lv_obj_add_state(s_new_record_button, LV_STATE_DISABLED);
    }
    if (show_retry) lv_obj_remove_flag(s_retry_button, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_retry_button, LV_OBJ_FLAG_HIDDEN);
    if (show_cancel) lv_obj_remove_flag(s_cancel_button, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_cancel_button, LV_OBJ_FLAG_HIDDEN);
}

static void show_answer_panel(bool visible)
{
    if (visible) {
        lv_obj_remove_flag(s_answer_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_record_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_new_record_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_answer_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_record_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_new_record_button, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_action_button(lv_obj_t *screen, lv_coord_t width, lv_coord_t height,
                                    lv_coord_t x, lv_coord_t y, lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_size(button, width, height);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    return button;
}

int voice_ui_init(const voice_ui_callbacks_t *callbacks)
{
    if (callbacks == NULL) return ESP_ERR_INVALID_ARG;
    s_callbacks = *callbacks;
    lv_display_t *display = bsp_display_start();
    if (display == NULL) return ESP_FAIL;
    bsp_display_brightness_set(80);
    if (!bsp_display_lock(0)) return ESP_ERR_TIMEOUT;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080B12), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xF5F7FA), 0);

    s_volume_down_button = make_action_button(screen, VOLUME_BUTTON_W, TOP_BUTTON_H,
                                               SAFE_X, TOP_BUTTON_Y, volume_down_event);
    lv_obj_t *volume_down_label = lv_label_create(s_volume_down_button);
    lv_label_set_text(volume_down_label, "VOL -");
    lv_obj_center(volume_down_label);
    s_volume_up_button = make_action_button(screen, VOLUME_BUTTON_W, TOP_BUTTON_H,
                                             SAFE_X + VOLUME_BUTTON_W + VOLUME_GAP,
                                             TOP_BUTTON_Y, volume_up_event);
    lv_obj_t *volume_up_label = lv_label_create(s_volume_up_button);
    lv_label_set_text(volume_up_label, "VOL +");
    lv_obj_center(volume_up_label);
    s_volume_label = lv_label_create(screen);
    lv_obj_set_width(s_volume_label, VOLUME_VALUE_W);
    lv_obj_align(s_volume_label, LV_ALIGN_TOP_LEFT, VOLUME_VALUE_X, TOP_BUTTON_Y + 10);
    lv_obj_set_style_text_align(s_volume_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_volume_label, "VOL --");
    s_audio_button = make_action_button(screen, AUDIO_BUTTON_W, TOP_BUTTON_H, AUDIO_BUTTON_X,
                                        TOP_BUTTON_Y, audio_event);
    s_audio_label = lv_label_create(s_audio_button);
    lv_obj_center(s_audio_label);
    lv_label_set_text(s_audio_label, "AUDIO");

    s_tailscale_label = lv_label_create(screen);
    lv_obj_set_width(s_tailscale_label, 160);
    lv_obj_align(s_tailscale_label, LV_ALIGN_TOP_MID, 0, TAILSCALE_Y);
    lv_obj_set_style_text_align(s_tailscale_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_tailscale_label, lv_color_hex(0x98A2B3), 0);
    lv_label_set_text(s_tailscale_label, "TS: idle");

    s_status = lv_label_create(screen);
    lv_obj_set_width(s_status, SAFE_W);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, TOP_Y);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_28, 0);

    s_detail = lv_label_create(screen);
    lv_obj_set_width(s_detail, SAFE_W);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, DETAIL_Y);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(0x98A2B3), 0);
    lv_obj_set_style_text_font(s_detail, &voice_font_20, 0);

    s_record_button = lv_button_create(screen);
    lv_obj_set_size(s_record_button, 230, 230);
    lv_obj_align(s_record_button, LV_ALIGN_CENTER, 0, RECORD_Y_OFFSET);
    lv_obj_set_style_shadow_width(s_record_button, 0, 0);
    lv_obj_set_style_radius(s_record_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0x7C3AED), 0);
    lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0xDC2626), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_record_button, record_event, LV_EVENT_ALL, NULL);
    s_record_label = lv_label_create(s_record_button);
    lv_label_set_text(s_record_label, "MANTENER\nPARA HABLAR");
    lv_obj_set_style_text_align(s_record_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_record_label, &lv_font_montserrat_20, 0);
    lv_obj_center(s_record_label);

    s_answer_panel = lv_obj_create(screen);
    lv_obj_set_size(s_answer_panel, SAFE_W, PANEL_H);
    lv_obj_align(s_answer_panel, LV_ALIGN_TOP_MID, 0, PANEL_Y);
    lv_obj_set_style_shadow_width(s_answer_panel, 0, 0);
    lv_obj_set_style_bg_color(s_answer_panel, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_width(s_answer_panel, 0, 0);
    lv_obj_set_flex_flow(s_answer_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_answer_panel, 12, 0);
    lv_obj_set_style_pad_all(s_answer_panel, 14, 0);
    lv_obj_set_scroll_dir(s_answer_panel, LV_DIR_VER);
    s_transcript = lv_label_create(s_answer_panel);
    lv_obj_set_width(s_transcript, SAFE_W - 28);
    lv_obj_set_style_text_font(s_transcript, &voice_font_20, 0);
    lv_obj_set_style_text_color(s_transcript, lv_color_hex(0x98A2B3), 0);
    lv_label_set_long_mode(s_transcript, LV_LABEL_LONG_WRAP);
    s_answer = lv_label_create(s_answer_panel);
    lv_obj_set_width(s_answer, SAFE_W - 28);
    lv_obj_set_style_text_font(s_answer, &voice_font_20, 0);
    lv_label_set_long_mode(s_answer, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_answer_panel, LV_OBJ_FLAG_HIDDEN);

    s_retry_button = make_action_button(screen, 130, ACTION_H, SAFE_X, ACTION_Y, retry_event);
    lv_obj_t *retry_label = lv_label_create(s_retry_button);
    lv_label_set_text(retry_label, "REINTENTAR");
    lv_obj_center(retry_label);
    lv_obj_add_flag(s_retry_button, LV_OBJ_FLAG_HIDDEN);

    s_cancel_button = make_action_button(screen, 130, ACTION_H, SAFE_X, ACTION_Y, cancel_event);
    lv_obj_t *cancel_label = lv_label_create(s_cancel_button);
    lv_label_set_text(cancel_label, "CANCELAR");
    lv_obj_center(cancel_label);
    lv_obj_add_flag(s_cancel_button, LV_OBJ_FLAG_HIDDEN);

    s_new_record_button = lv_button_create(screen);
    lv_obj_set_size(s_new_record_button, 180, ACTION_H);
    lv_obj_align(s_new_record_button, LV_ALIGN_TOP_RIGHT, -SAFE_X, ACTION_Y);
    lv_obj_set_style_shadow_width(s_new_record_button, 0, 0);
    lv_obj_set_style_bg_color(s_new_record_button, lv_color_hex(0x7C3AED), 0);
    lv_obj_set_style_bg_color(s_new_record_button, lv_color_hex(0xDC2626), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_new_record_button, record_event, LV_EVENT_ALL, NULL);
    s_new_record_label = lv_label_create(s_new_record_button);
    lv_label_set_text(s_new_record_label, "NUEVA PREGUNTA");
    lv_obj_center(s_new_record_label);
    lv_obj_add_flag(s_new_record_button, LV_OBJ_FLAG_HIDDEN);

    bsp_display_unlock();
    voice_ui_show_idle(false);
    ESP_LOGI(TAG, "UI initialized with Spanish UTF-8 labels: %s", SPANISH_GLYPHS);
    return ESP_OK;
}

void voice_ui_show_idle(bool connected)
{
    if (!bsp_display_lock(0)) return;
    cancel_record_release_timer();
    s_record_gesture_active = false;
    s_record_release_requested = false;
    s_ui_idle = true;
    show_answer_panel(false);
    lv_label_set_text(s_status, "Listo");
    lv_label_set_text(s_detail, connected ? "Conectado a Proxmox" : "Conectando al Wi-Fi…");
    lv_label_set_text(s_record_label, "MANTENER\nPARA HABLAR");
    set_interactive(true, false, false);
    bsp_display_unlock();
}

void voice_ui_show_recording(uint32_t duration_ms, uint8_t level)
{
    char detail[96];
    snprintf(detail, sizeof(detail), "%lu.%lus  •  nivel %u%%",
             (unsigned long)(duration_ms / 1000),
             (unsigned long)((duration_ms % 1000) / 100), level);
    if (!bsp_display_lock(0)) return;
    s_ui_idle = false;
    if (s_recording_from_new_button) {
        lv_obj_add_flag(s_answer_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_record_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_new_record_button, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_new_record_label, "SOLTAR PARA ENVIAR");
    } else {
        show_answer_panel(false);
    }
    lv_label_set_text(s_status, "Grabando");
    lv_label_set_text(s_detail, detail);
    lv_label_set_text(s_record_label, "SOLTAR\nPARA ENVIAR");
    set_interactive(true, false, true);
    bsp_display_unlock();
}

void voice_ui_show_working(const char *title, const char *detail)
{
    if (!bsp_display_lock(0)) return;
    s_ui_idle = false;
    show_answer_panel(false);
    lv_label_set_text(s_status, title);
    lv_label_set_text(s_detail, detail);
    lv_label_set_text(s_record_label, "…");
    set_interactive(false, false, true);
    bsp_display_unlock();
}

void voice_ui_show_playing(const char *detail)
{
    if (!bsp_display_lock(0)) return;
    s_ui_idle = false;
    show_answer_panel(true);
    lv_obj_add_flag(s_new_record_button, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_status, "Reproduciendo");
    lv_label_set_text(s_detail, detail != NULL ? detail : "Audio de Hermes");
    set_interactive(true, false, true);
    bsp_display_unlock();
}

void voice_ui_show_response(const char *transcript, const char *answer, bool truncated,
                            bool audio_enabled)
{
    char transcript_text[2200];
    snprintf(transcript_text, sizeof(transcript_text), "Vos: %s", transcript);
    if (!bsp_display_lock(0)) return;
    s_ui_idle = false;
    lv_label_set_text(s_status, "Hermes");
    lv_label_set_text(s_detail, audio_enabled ?
                      (truncated ? "Respuesta abreviada; texto y audio listos" : "Texto y audio listos") :
                      (truncated ? "Respuesta abreviada; audio apagado" : "Texto listo; audio apagado"));
    lv_label_set_text(s_transcript, transcript_text);
    lv_label_set_text(s_answer, answer);
    lv_label_set_text(s_new_record_label, "NUEVA PREGUNTA");
    show_answer_panel(true);
    set_interactive(true, false, false);
    bsp_display_unlock();
}

void voice_ui_show_audio_error(const char *message)
{
    if (!bsp_display_lock(0)) return;
    s_ui_idle = false;
    show_answer_panel(true);
    lv_label_set_text(s_status, "Texto listo");
    lv_label_set_text(s_detail, message != NULL ? message : "Audio no disponible");
    set_interactive(true, true, false);
    bsp_display_unlock();
}

void voice_ui_show_error(const char *message, bool can_retry)
{
    if (!bsp_display_lock(0)) return;
    s_ui_idle = false;
    show_answer_panel(false);
    lv_label_set_text(s_status, "No salió");
    lv_label_set_text(s_detail, message != NULL ? message : "Error inesperado");
    lv_label_set_text(s_record_label, "MANTENER\nPARA GRABAR DE NUEVO");
    set_interactive(true, can_retry, false);
    bsp_display_unlock();
}

void voice_ui_show_volume(uint8_t volume)
{
    s_volume = volume;
    if (!bsp_display_lock(0)) return;
    char text[32];
    snprintf(text, sizeof(text), "VOL %u%%", (unsigned)volume);
    lv_label_set_text(s_volume_label, text);
    bsp_display_unlock();
}

void voice_ui_set_audio_enabled(bool enabled)
{
    s_audio_enabled = enabled;
    if (!bsp_display_lock(0)) return;
    lv_label_set_text(s_audio_label, enabled ? "AUDIO ON" : "AUDIO OFF");
    bsp_display_unlock();
}

void voice_ui_set_tailscale_state(const char *state, bool connected)
{
    if (!bsp_display_lock(0)) return;
    lv_label_set_text_fmt(s_tailscale_label, "TS: %s", state != NULL ? state : "unknown");
    lv_obj_set_style_text_color(s_tailscale_label,
                                lv_color_hex(connected ? 0x22C55E : 0x98A2B3), 0);
    bool connecting = state != NULL &&
                      (strcmp(state, "wifi_wait") == 0 ||
                       strcmp(state, "connecting") == 0 ||
                       strcmp(state, "registering") == 0 ||
                       strcmp(state, "reconnecting") == 0);
    if (s_ui_idle && connected) {
        lv_label_set_text(s_detail, "Conectado a Proxmox");
    } else if (s_ui_idle && connecting) {
        lv_label_set_text(s_detail, "Conectando al Wi-Fi…");
    }
    bsp_display_unlock();
}

#include "voice_ui.h"

#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
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
static lv_obj_t *s_volume_button;
static lv_obj_t *s_volume_label;
static lv_obj_t *s_audio_button;
static lv_obj_t *s_audio_label;
static lv_obj_t *s_answer_panel;
static lv_obj_t *s_answer;
static lv_obj_t *s_transcript;
static bool s_recording_from_new_button;
static bool s_audio_enabled = true;
static uint8_t s_volume;

#define SAFE_X 16
#define SAFE_W (BSP_LCD_H_RES - (2 * SAFE_X))
#define TOP_Y 54
#define ACTION_Y (BSP_LCD_V_RES - 58)
#define PANEL_Y 116
#define PANEL_H (ACTION_Y - PANEL_Y - 8)

static void record_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED && s_callbacks.on_record_pressed != NULL) {
        s_recording_from_new_button = lv_event_get_target(event) == s_new_record_button;
        s_callbacks.on_record_pressed(s_callbacks.context);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) &&
               s_callbacks.on_record_released != NULL) {
        s_callbacks.on_record_released(s_callbacks.context);
        s_recording_from_new_button = false;
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

static void volume_event(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        s_callbacks.on_volume_pressed != NULL) {
        s_callbacks.on_volume_pressed(s_callbacks.context);
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

    s_volume_button = make_action_button(screen, 112, 38, SAFE_X, 8, volume_event);
    s_volume_label = lv_label_create(s_volume_button);
    lv_obj_center(s_volume_label);
    s_audio_button = make_action_button(screen, 126, 38, BSP_LCD_H_RES - SAFE_X - 126, 8,
                                        audio_event);
    s_audio_label = lv_label_create(s_audio_button);
    lv_obj_center(s_audio_label);

    s_status = lv_label_create(screen);
    lv_obj_set_width(s_status, SAFE_W);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, TOP_Y);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_28, 0);

    s_detail = lv_label_create(screen);
    lv_obj_set_width(s_detail, SAFE_W);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, TOP_Y + 42);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(0x98A2B3), 0);
    lv_obj_set_style_text_font(s_detail, &voice_font_20, 0);

    s_record_button = lv_button_create(screen);
    lv_obj_set_size(s_record_button, 230, 230);
    lv_obj_align(s_record_button, LV_ALIGN_CENTER, 0, 12);
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

    s_retry_button = make_action_button(screen, 130, 48, SAFE_X, ACTION_Y, retry_event);
    lv_obj_t *retry_label = lv_label_create(s_retry_button);
    lv_label_set_text(retry_label, "REINTENTAR");
    lv_obj_center(retry_label);
    lv_obj_add_flag(s_retry_button, LV_OBJ_FLAG_HIDDEN);

    s_cancel_button = make_action_button(screen, 130, 48, 150, ACTION_Y, cancel_event);
    lv_obj_t *cancel_label = lv_label_create(s_cancel_button);
    lv_label_set_text(cancel_label, "CANCELAR");
    lv_obj_center(cancel_label);
    lv_obj_add_flag(s_cancel_button, LV_OBJ_FLAG_HIDDEN);

    s_new_record_button = lv_button_create(screen);
    lv_obj_set_size(s_new_record_button, 180, 48);
    lv_obj_align(s_new_record_button, LV_ALIGN_TOP_RIGHT, -SAFE_X, ACTION_Y);
    lv_obj_set_style_shadow_width(s_new_record_button, 0, 0);
    lv_obj_set_style_bg_color(s_new_record_button, lv_color_hex(0x7C3AED), 0);
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
    show_answer_panel(true);
    lv_label_set_text(s_status, "Texto listo");
    lv_label_set_text(s_detail, message != NULL ? message : "Audio no disponible");
    set_interactive(true, true, false);
    bsp_display_unlock();
}

void voice_ui_show_error(const char *message, bool can_retry)
{
    if (!bsp_display_lock(0)) return;
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

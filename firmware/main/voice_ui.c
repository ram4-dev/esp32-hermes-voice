#include "voice_ui.h"

#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "voice_ui";
static voice_ui_callbacks_t s_callbacks;
static lv_obj_t *s_status;
static lv_obj_t *s_detail;
static lv_obj_t *s_record_button;
static lv_obj_t *s_new_record_button;
static lv_obj_t *s_new_record_label;
static lv_obj_t *s_record_label;
static lv_obj_t *s_retry_button;
static lv_obj_t *s_answer_panel;
static lv_obj_t *s_answer;
static lv_obj_t *s_transcript;
static bool s_recording_from_new_button;

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

static void set_interactive(bool enabled, bool show_retry)
{
    if (enabled) {
        lv_obj_remove_state(s_record_button, LV_STATE_DISABLED);
        lv_obj_remove_state(s_new_record_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_record_button, LV_STATE_DISABLED);
        lv_obj_add_state(s_new_record_button, LV_STATE_DISABLED);
    }
    if (show_retry) {
        lv_obj_remove_flag(s_retry_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_retry_button, LV_OBJ_FLAG_HIDDEN);
    }
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

esp_err_t voice_ui_init(const voice_ui_callbacks_t *callbacks)
{
    if (callbacks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_callbacks = *callbacks;
    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        return ESP_FAIL;
    }
    bsp_display_brightness_set(80);
    if (!bsp_display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080B12), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xF5F7FA), 0);

    s_status = lv_label_create(screen);
    lv_obj_set_width(s_status, 370);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_28, 0);

    s_detail = lv_label_create(screen);
    lv_obj_set_width(s_detail, 360);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(0x98A2B3), 0);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_16, 0);

    s_record_button = lv_button_create(screen);
    lv_obj_set_size(s_record_button, 250, 250);
    lv_obj_align(s_record_button, LV_ALIGN_CENTER, 0, 46);
    lv_obj_set_style_radius(s_record_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0x7C3AED), 0);
    lv_obj_set_style_bg_color(s_record_button, lv_color_hex(0xDC2626), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_record_button, record_event, LV_EVENT_ALL, NULL);

    s_record_label = lv_label_create(s_record_button);
    lv_label_set_text(s_record_label, "MANTENER\nPARA HABLAR");
    lv_obj_set_style_text_align(s_record_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_record_label, &lv_font_montserrat_20, 0);
    lv_obj_center(s_record_label);

    s_retry_button = lv_button_create(screen);
    lv_obj_set_size(s_retry_button, 170, 52);
    lv_obj_align(s_retry_button, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_add_event_cb(s_retry_button, retry_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *retry_label = lv_label_create(s_retry_button);
    lv_label_set_text(retry_label, "REINTENTAR");
    lv_obj_center(retry_label);
    lv_obj_add_flag(s_retry_button, LV_OBJ_FLAG_HIDDEN);

    s_new_record_button = lv_button_create(screen);
    lv_obj_set_size(s_new_record_button, 240, 58);
    lv_obj_align(s_new_record_button, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_bg_color(s_new_record_button, lv_color_hex(0x7C3AED), 0);
    lv_obj_set_style_bg_color(s_new_record_button, lv_color_hex(0xDC2626),
                              LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_new_record_button, record_event, LV_EVENT_ALL, NULL);
    s_new_record_label = lv_label_create(s_new_record_button);
    lv_label_set_text(s_new_record_label, "MANTENER PARA HABLAR");
    lv_obj_center(s_new_record_label);
    lv_obj_add_flag(s_new_record_button, LV_OBJ_FLAG_HIDDEN);

    s_answer_panel = lv_obj_create(screen);
    lv_obj_set_size(s_answer_panel, 370, 292);
    lv_obj_align(s_answer_panel, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_set_style_bg_color(s_answer_panel, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_width(s_answer_panel, 0, 0);
    lv_obj_set_flex_flow(s_answer_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_answer_panel, 16, 0);
    lv_obj_set_scroll_dir(s_answer_panel, LV_DIR_VER);

    s_transcript = lv_label_create(s_answer_panel);
    lv_obj_set_width(s_transcript, 330);
    lv_obj_set_style_text_color(s_transcript, lv_color_hex(0x98A2B3), 0);
    lv_label_set_long_mode(s_transcript, LV_LABEL_LONG_WRAP);

    s_answer = lv_label_create(s_answer_panel);
    lv_obj_set_width(s_answer, 330);
    lv_obj_set_style_text_font(s_answer, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(s_answer, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_answer_panel, LV_OBJ_FLAG_HIDDEN);

    bsp_display_unlock();
    voice_ui_show_idle(false);
    ESP_LOGI(TAG, "UI initialized");
    return ESP_OK;
}

void voice_ui_show_idle(bool connected)
{
    if (!bsp_display_lock(0)) return;
    show_answer_panel(false);
    lv_label_set_text(s_status, "Listo");
    lv_label_set_text(s_detail, connected ? "Conectado a Proxmox" : "Conectando al Wi-Fi…");
    lv_label_set_text(s_record_label, "MANTENER\nPARA HABLAR");
    set_interactive(true, false);
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
    set_interactive(true, false);
    bsp_display_unlock();
}

void voice_ui_show_working(const char *title, const char *detail)
{
    if (!bsp_display_lock(0)) return;
    show_answer_panel(false);
    lv_label_set_text(s_status, title);
    lv_label_set_text(s_detail, detail);
    lv_label_set_text(s_record_label, "…");
    set_interactive(false, false);
    bsp_display_unlock();
}

void voice_ui_show_response(const char *transcript, const char *answer, bool truncated)
{
    char transcript_text[2200];
    snprintf(transcript_text, sizeof(transcript_text), "Vos: %s", transcript);
    if (!bsp_display_lock(0)) return;
    lv_label_set_text(s_status, "Hermes");
    lv_label_set_text(s_detail, truncated ? "Respuesta abreviada para la pantalla" : "Respuesta lista");
    lv_label_set_text(s_transcript, transcript_text);
    lv_label_set_text(s_answer, answer);
    lv_label_set_text(s_new_record_label, "MANTENER PARA HABLAR");
    show_answer_panel(true);
    set_interactive(true, false);
    bsp_display_unlock();
}

void voice_ui_show_error(const char *message, bool can_retry)
{
    if (!bsp_display_lock(0)) return;
    show_answer_panel(false);
    lv_label_set_text(s_status, "No salió");
    lv_label_set_text(s_detail, message);
    lv_label_set_text(s_record_label, "MANTENER\nPARA GRABAR DE NUEVO");
    set_interactive(true, can_retry);
    bsp_display_unlock();
}

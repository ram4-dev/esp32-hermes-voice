from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).parents[1]
MAIN = ROOT / "main"
APP = (MAIN / "app_main.c").read_text()
PLAYER = (MAIN / "audio_player.c").read_text()
UI = (MAIN / "voice_ui.c").read_text()
UI_HEADER = (MAIN / "voice_ui.h").read_text()
WIFI = (MAIN / "wifi_manager.c").read_text()
KCONFIG = (MAIN / "Kconfig.projbuild").read_text()
DEFAULTS = (ROOT / "sdkconfig.defaults").read_text()


def test_volume_button_uses_nvs_and_audio_toggle_preserves_text() -> None:
    assert 'NVS_VOLUME_KEY = "volume"' in PLAYER
    assert "nvs_get_u8" in PLAYER
    assert "nvs_set_u8" in PLAYER
    assert "nvs_commit" in PLAYER
    assert 'NVS_AUDIO_ENABLED_KEY = "audio_enabled"' in PLAYER
    assert "audio_player_set_audio_enabled" in PLAYER
    assert "audio_player_get_audio_enabled" in PLAYER
    assert "on_volume_down_pressed" in UI_HEADER
    assert "on_volume_up_pressed" in UI_HEADER
    assert "on_audio_toggle_pressed" in UI_HEADER
    assert "voice_ui_show_response" in APP
    assert "if (s_audio_enabled && !token_cancelled(token))" in APP
    assert "Texto listo; audio apagado" in UI


def test_wifi_has_priority_fallback_and_reconnect_without_tracked_secrets() -> None:
    for config_name, kconfig_name in (
        ("CONFIG_VOICE_WIFI_PRIMARY_SSID", "VOICE_WIFI_PRIMARY_SSID"),
        ("CONFIG_VOICE_WIFI_PRIMARY_PASSWORD", "VOICE_WIFI_PRIMARY_PASSWORD"),
        ("CONFIG_VOICE_WIFI_SECONDARY_SSID", "VOICE_WIFI_SECONDARY_SSID"),
        ("CONFIG_VOICE_WIFI_SECONDARY_PASSWORD", "VOICE_WIFI_SECONDARY_PASSWORD"),
    ):
        assert config_name in WIFI
        assert kconfig_name in KCONFIG
    assert "WIFI_RETRY_LIMIT" in WIFI
    assert "try_next_network" in WIFI
    assert (
        'CONFIG_VOICE_BRIDGE_URL="https://hermes-server.tailfb789f.ts.net:8443"'
        in DEFAULTS
    )
    assert 'CONFIG_VOICE_WIFI_PRIMARY_SSID="Ramiro-2.4Ghz"' in DEFAULTS
    assert 'CONFIG_VOICE_WIFI_SECONDARY_SSID="Rama"' in DEFAULTS
    assert 'default "https://hermes-server.tailfb789f.ts.net:8443"' in KCONFIG
    assert "CONFIG_VOICE_WIFI_PASSWORD" not in DEFAULTS
    assert 'CONFIG_VOICE_WIFI_PRIMARY_PASSWORD="' not in DEFAULTS


def test_cancel_is_visible_for_all_active_states_and_late_callbacks_are_ignored() -> (
    None
):
    assert "on_cancel_pressed" in UI_HEADER
    assert 'lv_label_set_text(cancel_label, "CANCELAR")' in UI
    assert "set_interactive(true, false, true)" in UI
    assert "set_interactive(false, false, true)" in UI
    assert "volatile bool cancelled" in APP
    assert "token_cancelled" in APP
    assert "voice_cancel_cb_t" in (MAIN / "voice_client.h").read_text()
    assert "cancel_callback" in (MAIN / "voice_client.c").read_text()
    assert "audio_recorder_stop()" in APP
    assert "audio_player_stop()" in APP
    assert "finish_upload(token, false)" in APP
    assert "voice_ui_show_idle(wifi_manager_is_connected())" in APP


def test_spanish_glyph_font_and_layout_contract() -> None:
    glyphs = "áéíóúüñÁÉÍÓÚÜÑ¿¡"
    assert glyphs in UI
    font = (MAIN / "voice_font_20.c").read_text()
    for codepoint in ("U+00E1", "U+00F1", "U+00BF", "U+00A1"):
        assert codepoint in font
    assert '"voice_font_20.c"' in (MAIN / "CMakeLists.txt").read_text()
    assert "PANEL_H" in UI
    assert "ACTION_Y" in UI
    assert "SAFE_X 24" in UI
    assert "TOP_BUTTON_Y 24" in UI
    assert "VOLUME_BUTTON_W 76" in UI
    assert "AUDIO_BUTTON_X" in UI
    assert "lv_label_set_text(volume_down_label, \"VOL -\")" in UI
    assert "lv_label_set_text(volume_up_label, \"VOL +\")" in UI


def test_ui_actions_are_deferred_and_record_gesture_is_debounced() -> None:
    assert "UI_ACTION_QUEUE_DEPTH" in APP
    assert "xQueueSend(s_ui_actions, &action, 0)" in APP
    assert "ui_action_task" in APP
    assert "audio_player_set_audio_enabled" in APP
    assert "change_volume(-10)" in APP
    assert "change_volume(10)" in APP
    assert "LV_EVENT_PRESS_LOST" in UI
    assert "LV_EVENT_PRESSING" in UI
    assert "LV_EVENT_RELEASED" in UI
    assert "static void discrete_button_event" in UI
    assert "state->armed = true" in UI
    assert "state->armed = false" in UI
    assert "code == LV_EVENT_PRESS_LOST" in UI
    assert "lv_obj_add_event_cb(button, discrete_button_event, LV_EVENT_ALL, state)" in UI
    assert "RECORD_RELEASE_GRACE_MS 180" in UI
    assert "lv_timer_set_repeat_count" in UI
    assert "Preparando micrófono…" in UI
    assert "set_recording_visual(true)" in UI
    assert "LV_OPA_COVER" in UI
    assert "if (s_record_gesture_active) return;" in UI
    assert "s_record_release_requested = false;" in UI
    assert "cancel_record_release_timer();" in UI

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
    assert "on_volume_pressed" in UI_HEADER
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

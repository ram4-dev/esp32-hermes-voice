from pathlib import Path

ROOT = Path(__file__).parents[1]
COMPONENT = ROOT / "components" / "microlink"
MAIN = ROOT / "main"


def test_microlink_contract_is_wifi_only_and_bounded() -> None:
    kconfig = (COMPONENT / "Kconfig").read_text()
    defaults = (ROOT / "sdkconfig.defaults").read_text()
    cmake = (COMPONENT / "CMakeLists.txt").read_text()

    assert 'default 11' in kconfig
    assert 'default 8' in kconfig
    assert 'default 64' in kconfig
    assert 'CONFIG_ML_DERP_REGION=11' in defaults
    assert 'CONFIG_ML_MAX_PEERS=8' in defaults
    assert 'CONFIG_ML_H2_BUFFER_SIZE_KB=64' in defaults
    assert 'CONFIG_ML_JSON_BUFFER_SIZE_KB=64' in defaults
    assert 'CONFIG_ML_DEVICE_NAME="esp32-hermes"' in defaults
    assert 'CONFIG_ML_TAILSCALE_AUTH_KEY=""' in defaults
    assert 'CONFIG_MBEDTLS_CHACHA20_C=y' in defaults
    assert 'CONFIG_MBEDTLS_POLY1305_C=y' in defaults
    assert 'CONFIG_MBEDTLS_CHACHAPOLY_C=y' in defaults
    assert 'select MBEDTLS_CHACHA20_C' in kconfig
    assert 'select MBEDTLS_POLY1305_C' in kconfig
    assert 'select MBEDTLS_CHACHAPOLY_C' in kconfig
    internal = (COMPONENT / "include/microlink_internal.h").read_text()
    assert '"derp11e.tailscale.com"' in internal
    assert 'derp9' not in internal
    assert 'esp_http_server' not in cmake
    assert 'ml_config_httpd.c' not in cmake
    assert 'ml_cellular.c' not in cmake
    assert 'ML_ENABLE_CELLULAR' not in kconfig
    assert 'ML_ENABLE_CONFIG_HTTPD' not in kconfig


def test_required_upstream_fixes_are_present_and_provenanced() -> None:
    rx = (ROOT / "components" / "wireguard_lwip/src/wireguardif.c").read_text()
    peers = (COMPONENT / "src/ml_wg_mgr.c").read_text()
    provenance = (COMPONENT / "PROVENANCE.md").read_text()

    assert 'device->netif->input(pbuf, device->netif)' in rx
    assert 'ip_input(pbuf, device->netif)' not in rx
    assert 'ip_addr_set_any(false, &wg_peer.endpoint_ip);' in peers
    assert 'PR #20' in provenance and 'e74be46469c80089cef935d79667f4cca2fc00b0' in provenance
    assert 'PR #21' in provenance and 'da55beb7e8c13be2deaeacdc166c54d135d86235' in provenance


def test_wifi_ip_events_start_and_rebind_without_event_loop_blocking() -> None:
    wifi = (MAIN / "wifi_manager.c").read_text()
    link = (MAIN / "tailscale_link.c").read_text()
    app = (MAIN / "app_main.c").read_text()

    assert 'WIFI_MANAGER_GOT_IP' in wifi
    assert 's_event_callback(WIFI_MANAGER_GOT_IP' in wifi
    assert 'xQueueSend(s_events, &message, 0)' in link
    assert 'microlink_start(s_microlink)' in link
    assert 'microlink_rebind(s_microlink)' in link
    assert 'tailscale_link_init()' in app
    assert 'wifi_manager_set_event_callback' in app
    assert 'voice_ui_set_tailscale_state' in (MAIN / "voice_ui.c").read_text()


def test_lifecycle_recovery_and_same_network_rebind_are_explicit() -> None:
    link = (MAIN / "tailscale_link.c").read_text()
    ui = (MAIN / "voice_ui.c").read_text()

    assert 'static bool s_wifi_rebind_pending' in link
    assert 's_wifi_rebind_pending = true' in link
    assert 'if (s_wifi_rebind_pending || network_index != s_active_wifi)' in link
    assert 's_wifi_rebind_pending = false' in link
    assert 'microlink_destroy(s_microlink)' in link
    assert 's_microlink = NULL' in link
    assert 'static bool s_ui_idle' in ui
    assert 'if (s_ui_idle && connected)' in ui
    assert 'Conectado a Proxmox' in ui
    assert 's_ui_idle = false' in ui


def test_voice_endpoint_and_conditional_vpn_interface_binding() -> None:
    client = (MAIN / "voice_client.c").read_text()
    defaults = (ROOT / "sdkconfig.defaults").read_text()
    link = (MAIN / "tailscale_link.c").read_text()

    assert 'CONFIG_VOICE_BRIDGE_URL="https://hermes-server.tailfb789f.ts.net:8443"' in defaults
    assert 'config->if_name = vpn_ifreq' in client
    assert 'tailscale_link_bind_vpn_route' in client
    assert '0xFFC00000UL' in link and '0x64400000UL' in link
    assert 'ml0' in link
    assert 'CONFIG_VOICE_DEVICE_TOKEN[0] == \'\\0\'' in (MAIN / "app_main.c").read_text()

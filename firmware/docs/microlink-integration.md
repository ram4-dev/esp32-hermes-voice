# MicroLink integration (FASE 1–4)

## FASE 1 — Vendor and build boundary

`components/microlink` vendors the Wi-Fi-only MicroLink snapshot identified in
`components/microlink/PROVENANCE.md`. The nested `wireguard_lwip` component is
built locally. Cellular, AT sockets, network failover, and the MicroLink HTTP
configuration server are not enabled or compiled.

The firmware uses the following fixed defaults:

- `CONFIG_ML_DERP_REGION=11` (São Paulo)
- `CONFIG_ML_MAX_PEERS=8`
- `CONFIG_ML_H2_BUFFER_SIZE_KB=64`
- `CONFIG_ML_JSON_BUFFER_SIZE_KB=64`
- `CONFIG_ML_DEVICE_NAME="esp32-hermes"`
- `CONFIG_ML_TAILSCALE_AUTH_KEY=""` in tracked defaults; a real key belongs
  only in ignored local `sdkconfig`/menuconfig and is never logged
- `CONFIG_VOICE_BRIDGE_URL="https://hermes-server.tailfb789f.ts.net:8443"`

The control plane's HomeDERP is accepted when supplied; region 11 remains the
configured PreferredDERP/fallback. This keeps Tailscale's advertised and
actual region selection consistent with the DERP map while making the desired
São Paulo region explicit in Kconfig.

## FASE 2 — Lifecycle and Wi-Fi integration

`main/tailscale_link.c` owns one MicroLink instance and a small worker queue.
The Wi-Fi event callback only enqueues `GOT_IP`/`LOST_IP` and therefore never
waits on MicroLink, lwIP, or LVGL. MicroLink is initialized and started after
the first station `GOT_IP` event. When `wifi_manager` moves from primary to
secondary (or back), the next `GOT_IP` causes `microlink_rebind()`; the
WireGuard keys, peer state, VPN IP, and DISCO state remain alive.

The app-visible status API is `tailscale_link_get_state()`,
`tailscale_link_is_connected()`, `tailscale_link_get_vpn_ip()`, and
`tailscale_link_active_wifi()`. State changes are logged without credentials.
The existing idle UI uses Tailscale readiness for its connected indication;
voice work remains on its own task and is not blocked by VPN registration.

## FASE 3 — Voice HTTP routing

ESP-IDF 5.5.4's `esp_http_client_config_t` has `if_name`; its implementation
uses `SO_BINDTODEVICE` when supplied. The normal lwIP IPv4 route lookup also
selects the MicroLink netif (`ml0`) for destinations in `100.64.0.0/10`, since
the VPN netif is configured with the assigned `100.x` address and `/10`
netmask and is inserted in the lwIP netif list.

`voice_client.c` resolves the configured voice host in its worker task. If the
result is in `100.64.0.0/10`, it conditionally supplies `if_name="ml0"` as a
small defence against route-order changes. Public addresses for the unchanged
Funnel hostname are deliberately not bound and use normal Wi-Fi routing. No
custom HTTP client or global route mutation is needed.

This decision is covered by the contract tests in
`tests/test_microlink_integration.py`. The network path still requires a
running tailnet and cannot be proven by a host-only test.

## FASE 4 — Safety fixes and observability

The vendored WireGuard RX path enters lwIP through `netif->input` and transfers
pbuf ownership only when the mailbox accepts it. Peer endpoints from
MapResponse stay blank until a direct DISCO path is validated. This preserves
DERP operation for unvalidated/CGNAT peers and prevents the known pbuf race.

No Tailscale or voice secret is printed. Normal logs contain only lifecycle
state, peer count, selected Wi-Fi index, and VPN IP. This integration does not
flash hardware or alter the voice endpoint.

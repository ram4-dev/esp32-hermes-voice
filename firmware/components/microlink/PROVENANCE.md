# MicroLink vendor provenance

This directory vendors the Wi-Fi-only subset of MicroLink V2 from:

- Repository: https://github.com/CamM2325/microlink
- Base commit: `216da3300f0493b0860247d43f7af5ce29df63a5`
- License: `LICENSE.microlink` (MIT); WireGuard-lwIP license is in
  `../wireguard_lwip/LICENSE`; the X25519 notice is in `X25519-LICENSE.txt`.

The cellular modem, AT socket bridge, network-switching module, and HTTP
configuration server are intentionally not part of this integration. The
Kconfig surface is correspondingly limited to Wi-Fi MicroLink operation.

## Required upstream fixes

The base snapshot did not contain both verified fixes, so they are reproduced
in the vendored sources with their original commits recorded here:

- PR #20, commit `e74be46469c80089cef935d79667f4cca2fc00b0`,
  `../wireguard_lwip/src/wireguardif.c`: decrypted RX now calls
  `device->netif->input(pbuf, device->netif)` and transfers ownership only on
  `ERR_OK`, instead of calling `ip_input()` directly. This serializes entry
  into lwIP through `tcpip_thread` and avoids concurrent pbuf/PCB access.
- PR #21, commit `da55beb7e8c13be2deaeacdc166c54d135d86235`,
  `src/ml_wg_mgr.c`: peer installation leaves `endpoint_ip` empty and port
  zero until a DISCO-validated path is available. The previous code installed
  `Endpoints[0]` from MapResponse as if it were validated, which could
  black-hole outbound packets behind CGNAT.

The two changes are intentionally kept as small local diffs against the
specified base commit; no commit, reset, checkout, or upstream history was
introduced into this repository.

The hardcoded DERP/STUN fallback is `derp11e.tailscale.com` (region 11,
São Paulo); no region-9 fallback remains. The vendored `strncpy` calls that
trigger `-Werror=stringop-truncation` in ESP-IDF 5.5.4 were changed to bounded
`strlcpy` calls. These are build-only safety/compatibility fixes and do not
alter the MicroLink protocol.

The vendored files under `firmware/components/` use LF line endings. This is
an explicit line-ending normalization only; source content and formatting
otherwise match the upstream snapshot plus the deliberate integration fixes
listed above.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "microlink.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TAILSCALE_LINK_WIFI_GOT_IP = 0,
    TAILSCALE_LINK_WIFI_LOST_IP,
} tailscale_link_wifi_event_t;

/* Called from the Wi-Fi event loop. It only queues work and never blocks. */
void tailscale_link_on_wifi_event(tailscale_link_wifi_event_t event,
                                  uint8_t network_index, void *context);

/* Creates the private worker. MicroLink is initialized only after GOT_IP. */
esp_err_t tailscale_link_init(void);

microlink_state_t tailscale_link_get_state(void);
bool tailscale_link_is_connected(void);
uint32_t tailscale_link_get_vpn_ip(void);
uint8_t tailscale_link_active_wifi(void);
const char *tailscale_link_state_name(microlink_state_t state);

/*
 * Fill an esp_http_client-compatible interface name only when host_ip is in
 * Tailscale's 100.64.0.0/10 CGNAT range. Returns false for public endpoints.
 */
bool tailscale_link_bind_vpn_route(uint32_t host_ip, char *if_name, size_t capacity);

#ifdef __cplusplus
}
#endif

#include "tailscale_link.h"

#include "sdkconfig.h"
#include <string.h>

#include "esp_log.h"
#include "voice_ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TAILSCALE_LINK_QUEUE_DEPTH 4
#define TAILSCALE_NETIF_NAME "ml0"

static const char *TAG = "tailscale_link";

typedef struct {
    tailscale_link_wifi_event_t event;
    uint8_t network_index;
} wifi_event_message_t;

static QueueHandle_t s_events;
static TaskHandle_t s_task;
static microlink_t *s_microlink;
static volatile microlink_state_t s_state = ML_STATE_IDLE;
static volatile uint32_t s_vpn_ip;
static volatile uint8_t s_active_wifi;
static bool s_wifi_rebind_pending;

static void publish_state(microlink_state_t state)
{
    s_state = state;
    voice_ui_set_tailscale_state(tailscale_link_state_name(state), state == ML_STATE_CONNECTED);
}

static void state_changed(microlink_t *ml, microlink_state_t state, void *context)
{
    (void)ml;
    (void)context;
    publish_state(state);
    ESP_LOGI(TAG, "state=%s vpn=%08lx peers=%d wifi=%u",
             tailscale_link_state_name(state), (unsigned long)s_vpn_ip,
             s_microlink != NULL ? microlink_get_peer_count(s_microlink) : 0,
             (unsigned)s_active_wifi);
}

static void start_after_ip(uint8_t network_index)
{
    if (s_microlink == NULL) {
        if (CONFIG_ML_TAILSCALE_AUTH_KEY[0] == '\0') {
            publish_state(ML_STATE_ERROR);
            ESP_LOGW(TAG, "Tailscale auth key is not configured; VPN integration disabled");
            return;
        }

        microlink_config_t config = {
            .auth_key = CONFIG_ML_TAILSCALE_AUTH_KEY,
            .device_name = CONFIG_ML_DEVICE_NAME,
            .enable_derp = true,
            .enable_stun = true,
            .enable_disco = true,
            .max_peers = CONFIG_ML_MAX_PEERS,
            .wifi_tx_power_dbm = 0,
        };
        s_microlink = microlink_init(&config);
        if (s_microlink == NULL) {
            publish_state(ML_STATE_ERROR);
            ESP_LOGE(TAG, "MicroLink initialization failed");
            return;
        }
        microlink_set_state_callback(s_microlink, state_changed, NULL);
        esp_err_t result = microlink_start(s_microlink);
        if (result != ESP_OK) {
            publish_state(ML_STATE_ERROR);
            ESP_LOGE(TAG, "MicroLink start failed: %s", esp_err_to_name(result));
            /* Do not retain a handle whose tasks/sockets could not start.
             * The next GOT_IP can create a fresh, recoverable instance. */
            microlink_destroy(s_microlink);
            s_microlink = NULL;
            return;
        }
        s_active_wifi = network_index;
        s_wifi_rebind_pending = false;
        publish_state(microlink_get_state(s_microlink));
        ESP_LOGI(TAG, "MicroLink started after Wi-Fi network %u obtained an IP",
                 (unsigned)(network_index + 1));
        return;
    }

    /* A GOT_IP after any LOST_IP is a new Wi-Fi lifecycle, even when the
     * SSID/index is unchanged. Consume the pending transition before calling
     * MicroLink so duplicate GOT_IP notifications do not rebind twice. */
    if (s_wifi_rebind_pending || network_index != s_active_wifi) {
        s_wifi_rebind_pending = false;
        s_active_wifi = network_index;
        publish_state(ML_STATE_RECONNECTING);
        ESP_LOGI(TAG, "Wi-Fi became ready; rebinding MicroLink to network %u",
                 (unsigned)(network_index + 1));
        esp_err_t result = microlink_rebind(s_microlink);
        if (result != ESP_OK) {
            publish_state(ML_STATE_ERROR);
            ESP_LOGE(TAG, "MicroLink rebind failed: %s", esp_err_to_name(result));
            return;
        }
        publish_state(microlink_get_state(s_microlink));
    }
}

static void link_task(void *argument)
{
    (void)argument;
    wifi_event_message_t message;
    while (xQueueReceive(s_events, &message, portMAX_DELAY) == pdTRUE) {
        if (message.event == TAILSCALE_LINK_WIFI_GOT_IP) {
            start_after_ip(message.network_index);
        } else if (message.event == TAILSCALE_LINK_WIFI_LOST_IP) {
            s_wifi_rebind_pending = true;
            publish_state(ML_STATE_RECONNECTING);
            ESP_LOGW(TAG, "Wi-Fi network %u lost its IP; awaiting replacement",
                     (unsigned)(message.network_index + 1));
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

void tailscale_link_on_wifi_event(tailscale_link_wifi_event_t event,
                                  uint8_t network_index, void *context)
{
    (void)context;
    if (s_events == NULL) return;
    wifi_event_message_t message = { .event = event, .network_index = network_index };
    /* The Wi-Fi event loop must never wait on MicroLink or the UI. */
    if (xQueueSend(s_events, &message, 0) != pdTRUE) {
        ESP_LOGW(TAG, "dropping Wi-Fi event while MicroLink worker is busy");
    }
}

esp_err_t tailscale_link_init(void)
{
    if (s_events != NULL) return ESP_ERR_INVALID_STATE;
    s_events = xQueueCreate(TAILSCALE_LINK_QUEUE_DEPTH, sizeof(wifi_event_message_t));
    if (s_events == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(link_task, "tailscale_link", 6144, NULL, 4, &s_task) != pdPASS) {
        vQueueDelete(s_events);
        s_events = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

microlink_state_t tailscale_link_get_state(void)
{
    if (s_microlink != NULL) {
        s_state = microlink_get_state(s_microlink);
        s_vpn_ip = microlink_get_vpn_ip(s_microlink);
    }
    return s_state;
}

bool tailscale_link_is_connected(void)
{
    return s_microlink != NULL && microlink_is_connected(s_microlink);
}

uint32_t tailscale_link_get_vpn_ip(void)
{
    if (s_microlink != NULL) s_vpn_ip = microlink_get_vpn_ip(s_microlink);
    return s_vpn_ip;
}

uint8_t tailscale_link_active_wifi(void)
{
    return s_active_wifi;
}

const char *tailscale_link_state_name(microlink_state_t state)
{
    switch (state) {
    case ML_STATE_IDLE: return "idle";
    case ML_STATE_WIFI_WAIT: return "wifi_wait";
    case ML_STATE_CONNECTING: return "connecting";
    case ML_STATE_REGISTERING: return "registering";
    case ML_STATE_CONNECTED: return "connected";
    case ML_STATE_RECONNECTING: return "reconnecting";
    case ML_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

bool tailscale_link_bind_vpn_route(uint32_t host_ip, char *if_name, size_t capacity)
{
    if (if_name == NULL || capacity == 0 || !tailscale_link_is_connected()) return false;
    /* 100.64.0.0/10, in host byte order. Do not bind public Funnel traffic. */
    if ((host_ip & 0xFFC00000UL) != 0x64400000UL) return false;
    if (strlen(TAILSCALE_NETIF_NAME) + 1 > capacity) return false;
    strlcpy(if_name, TAILSCALE_NETIF_NAME, capacity);
    return true;
}

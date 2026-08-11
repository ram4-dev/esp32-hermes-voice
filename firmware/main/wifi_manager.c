#include "wifi_manager.h"

#include "sdkconfig.h"
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_RETRY_LIMIT 3
#define WIFI_PRIMARY 0
#define WIFI_FALLBACK 1

static const char *TAG = "wifi_manager";
static EventGroupHandle_t s_events;
static uint8_t s_network_index;
static uint8_t s_failures;

static bool network_configured(uint8_t index)
{
    if (index == WIFI_PRIMARY) {
        return CONFIG_VOICE_WIFI_PRIMARY_SSID[0] != '\0';
    }
    return CONFIG_VOICE_WIFI_SECONDARY_SSID[0] != '\0';
}

static esp_err_t apply_network(uint8_t index)
{
    wifi_config_t config = {0};
    const char *ssid = index == WIFI_PRIMARY ? CONFIG_VOICE_WIFI_PRIMARY_SSID
                                             : CONFIG_VOICE_WIFI_SECONDARY_SSID;
    const char *password = index == WIFI_PRIMARY ? CONFIG_VOICE_WIFI_PRIMARY_PASSWORD
                                                 : CONFIG_VOICE_WIFI_SECONDARY_PASSWORD;
    if (!network_configured(index)) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result == ESP_OK) {
        s_network_index = index;
        s_failures = 0;
        ESP_LOGI(TAG, "trying configured Wi-Fi network %u", (unsigned)(index + 1));
    }
    return result;
}

static void try_next_network(void)
{
    uint8_t next = s_network_index == WIFI_PRIMARY ? WIFI_FALLBACK : WIFI_PRIMARY;
    if (network_configured(next)) {
        if (apply_network(next) != ESP_OK) {
            ESP_LOGW(TAG, "could not apply configured Wi-Fi fallback");
        }
    } else {
        s_failures = 0;
    }
}

static void wifi_event_handler(
    void *context, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)context;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        ++s_failures;
        if (s_failures >= WIFI_RETRY_LIMIT) {
            try_next_network();
        }
        esp_wifi_connect();
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d); retry=%u network=%u",
                 disconnected != NULL ? disconnected->reason : -1,
                 (unsigned)s_failures, (unsigned)(s_network_index + 1));
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_failures = 0;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected on network %u", (unsigned)(s_network_index + 1));
    }
}

esp_err_t wifi_manager_start(void)
{
    if (!network_configured(WIFI_PRIMARY) && !network_configured(WIFI_FALLBACK)) {
        ESP_LOGE(TAG, "configure at least one Wi-Fi network in local sdkconfig");
        return ESP_ERR_INVALID_STATE;
    }
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
        TAG, "Wi-Fi handler failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
        TAG, "IP handler failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode failed");
    s_network_index = network_configured(WIFI_PRIMARY) ? WIFI_PRIMARY : WIFI_FALLBACK;
    ESP_RETURN_ON_ERROR(apply_network(s_network_index), TAG, "Wi-Fi config failed");
    return esp_wifi_start();
}

bool wifi_manager_wait_connected(uint32_t timeout_ms)
{
    if (s_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_connected(void)
{
    return s_events != NULL &&
           (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0;
}

uint8_t wifi_manager_active_network(void)
{
    return (uint8_t)(s_network_index + 1);
}

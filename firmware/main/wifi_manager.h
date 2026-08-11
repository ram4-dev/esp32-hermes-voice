#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WIFI_MANAGER_GOT_IP = 0,
    WIFI_MANAGER_LOST_IP,
} wifi_manager_event_t;

typedef void (*wifi_manager_event_cb_t)(wifi_manager_event_t event,
                                         uint8_t network_index, void *context);

esp_err_t wifi_manager_start(void);
void wifi_manager_set_event_callback(wifi_manager_event_cb_t callback, void *context);
bool wifi_manager_wait_connected(uint32_t timeout_ms);
bool wifi_manager_is_connected(void);
uint8_t wifi_manager_active_network(void);

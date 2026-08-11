idf_component_register(
    SRCS
        "app_main.c"
        "audio_recorder.c"
        "audio_player.c"
        "voice_client.c"
        "voice_ui.c"
        "voice_font_20.c"
        "wav.c"
        "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_codec_dev
        esp_event
        esp_http_client
        esp_netif
        esp-tls
        esp_timer
        esp_wifi
        json
        nvs_flash
)

if(CONFIG_VOICE_BRIDGE_EMBED_LOCAL_CA)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/certs/server_root_ca.pem")
        message(FATAL_ERROR
            "VOICE_BRIDGE_EMBED_LOCAL_CA is enabled, but "
            "firmware/main/certs/server_root_ca.pem is missing")
    endif()
    target_add_binary_data(${COMPONENT_LIB} "certs/server_root_ca.pem" TEXT)
endif()

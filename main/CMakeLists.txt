idf_component_register(
    SRCS "my_websocket.c"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_netif esp_wifi esp_websocket_client nvs_flash
)

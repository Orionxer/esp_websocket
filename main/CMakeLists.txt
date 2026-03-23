idf_component_register(
    SRCS "my_websocket.c"
    EMBED_TXTFILES "AAA-Certificate-Services.pem"
    EMBED_FILES "starship_1mb.mp4"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_netif esp_wifi esp_websocket_client mbedtls nvs_flash
)

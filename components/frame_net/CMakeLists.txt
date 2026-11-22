idf_component_register(
    SRCS "net.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_core frame_storage esp_wifi esp_netif esp_event esp_http_client
             esp_timer nvs_flash
)

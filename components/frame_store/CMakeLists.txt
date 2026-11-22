idf_component_register(
    SRCS
        "store.c"
        "catalog.c"
        "install.c"
        "api.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_core frame_pure frame_storage frame_net frame_ota frame_log
             esp_http_client esp_http_server esp_timer esp_partition nvs_flash
             esp_app_format app_update
)

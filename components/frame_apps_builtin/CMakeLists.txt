idf_component_register(
    SRCS
        "apps.c"
        "launcher/launcher.c"
        "logs/logs_app.c"
        "settings/settings_app.c"
        "store/store_app.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_core frame_ui frame_log frame_storage frame_net frame_store
             esp_wifi esp_netif esp_event
)

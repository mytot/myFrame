idf_component_register(
    SRCS
        "event.c"
        "app_manager.c"
        "watchdog.c"
        "power.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_pure esp_timer esp_system nvs_flash
)

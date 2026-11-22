idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES frame_board frame_core frame_display frame_input frame_log frame_storage
             frame_net frame_ota frame_store frame_ui frame_wasm frame_shell
             frame_apps_builtin nvs_flash esp_system esp_timer esp_wifi
)

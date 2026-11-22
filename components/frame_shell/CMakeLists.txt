idf_component_register(
    SRCS "shell.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_core frame_ui frame_display frame_wasm frame_ota frame_log frame_apps_builtin
             esp_system esp_timer
)

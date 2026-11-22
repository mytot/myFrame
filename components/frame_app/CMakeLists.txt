idf_component_register(
    SRCS "app_runtime.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_core frame_display frame_input frame_log frame_storage frame_net
             frame_ota frame_ui nvs_flash esp_system esp_timer
)
# Deliberately NOT depending on frame_store, frame_wasm or frame_shell: a native
# app image is ~1.5 MB and every component linked in eats into that budget.

idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_core frame_display frame_input frame_log frame_storage lvgl esp_timer
)

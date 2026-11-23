idf_component_register(
    SRCS "frame_display.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_core esp_lcd esp_driver_gpio esp_driver_ledc esp_timer
             esp_mm lvgl
)

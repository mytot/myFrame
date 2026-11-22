idf_component_register(
    SRCS "frame_input.c" "gt911.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_core esp_driver_i2c esp_driver_gpio esp_timer lvgl
)

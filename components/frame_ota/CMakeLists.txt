idf_component_register(
    SRCS "ota.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_core frame_pure frame_storage frame_log app_update esp_partition esp_ota_ops
             bootloader_support nvs_flash esp_timer
)

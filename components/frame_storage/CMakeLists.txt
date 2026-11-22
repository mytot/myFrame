idf_component_register(
    SRCS "storage.c" "settings.c" "registry.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_core frame_pure nvs_flash fatfs wear_levelling sdmmc
)

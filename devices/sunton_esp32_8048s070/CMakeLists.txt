# The component is always called frame_board, whichever directory it lives in,
# so that main/ and the HAL can depend on a stable name.
set(COMPONENT_NAME frame_board)

idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "."
    REQUIRES frame_core driver esp_lcd
)

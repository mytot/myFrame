cmake_minimum_required(VERSION 3.22.1)

# ---------------------------------------------------------------------------
# myFrame — FrameOS shell image
#
#   idf.py build                                   (default board)
#   idf.py -DBOARD=sunton_esp32_8048s070 build
#   python3 tools/build.py --release --key keys/release/store_root.pub
#
# The board is a build-time choice. It selects sdkconfig defaults, the partition
# table and the device HAL; the firmware source is identical for every board.
# Shared glue (and the native-app variant of this file) lives in
# tools/cmake/frame_project.cmake.
# ---------------------------------------------------------------------------

set(FRAME_ROLE "SHELL")
include(${CMAKE_CURRENT_LIST_DIR}/tools/cmake/frame_project.cmake)
frame_project_setup(${CMAKE_CURRENT_LIST_DIR})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(myframe)

frame_project_finish()

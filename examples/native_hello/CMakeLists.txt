cmake_minimum_required(VERSION 3.22.1)

# ---------------------------------------------------------------------------
# Counter — a native Frame app (complete ESP-IDF image for an OTA slot).
#
#   idf.py -B build build
#   python3 ../../tools/pack.py --kind native --manifest manifest.json \
#           --payload build/frame_native_hello.bin --publisher ../../keys/dev/publisher
#
# This image is ~1.5 MB because it carries its own copy of the display/UI
# stack; that is the price of a native app and the reason managed apps exist
# for everything that does not need a peripheral.
# ---------------------------------------------------------------------------

set(FRAME_ROLE "APP")
include(${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/frame_project.cmake)
frame_project_setup(${CMAKE_CURRENT_LIST_DIR}/../..)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(frame_native_hello)

frame_project_finish()

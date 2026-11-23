# tools/cmake/frame_project.cmake — shared project glue for FrameOS images.
#
# Two kinds of image are built from this tree and they must agree on board
# selection, sdkconfig layering, the partition table and the generated headers:
#
#   the shell          (repository root)        -> main/app_main.c
#   a native app       (examples/native_hello)  -> its own main/, links frame_app
#
# Usage, from a project's top-level CMakeLists.txt:
#
#   cmake_minimum_required(VERSION 3.22.1)
#   include(<repo>/tools/cmake/frame_project.cmake)
#   frame_project_setup(<repo root>)
#   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
#   project(<name>)
#   frame_project_finish()
#
# Variables the project may set before frame_project_setup():
#   FRAME_ROLE              SHELL (default) or APP — controls which checks run
#   EXTRA_SDKCONFIG_DEFAULTS  extra sdkconfig.defaults files, layered last
#   BOARD                   board id (default sunton_esp32_8048s070)

set(FRAME_DEFAULT_BOARD "sunton_esp32_8048s070")

function(frame_project_setup repo_root)
    # Runs before IDF's project.cmake defines PYTHON, so find the interpreter
    # ourselves; if IDF already set it, use that.
    if(NOT PYTHON)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        set(PYTHON "${Python3_EXECUTABLE}")
    endif()

    set(FRAME_ROOT "${repo_root}" PARENT_SCOPE)
    set(FRAME_ROOT "${repo_root}")

    if(NOT BOARD)
        set(BOARD "${FRAME_DEFAULT_BOARD}" CACHE STRING "FrameOS target board id")
    endif()
    set(FRAME_BOARD_DIR "${repo_root}/devices/${BOARD}")

    if(NOT EXISTS "${FRAME_BOARD_DIR}/device.json")
        file(GLOB _boards RELATIVE "${repo_root}/devices" "${repo_root}/devices/*")
        string(REPLACE ";" "\n  " _board_list "${_boards}")
        message(FATAL_ERROR
            "Unknown FrameOS board '${BOARD}'. Expected devices/${BOARD}/device.json.\n"
            "Available boards:\n  ${_board_list}")
    endif()
    message(STATUS "FrameOS board: ${BOARD}  role: ${FRAME_ROLE}")

    if(NOT FRAME_ROLE)
        set(FRAME_ROLE "SHELL")
    endif()

    # sdkconfig defaults are layered: project-wide, then the board's, then any
    # the caller added. Later files win.
    set(SDKCONFIG_DEFAULTS
        "${repo_root}/sdkconfig.defaults"
        "${FRAME_BOARD_DIR}/sdkconfig.defaults"
        ${EXTRA_SDKCONFIG_DEFAULTS}
        PARENT_SCOPE)
    set(PARTITION_TABLE_CUSTOM_FILENAME "${FRAME_BOARD_DIR}/partitions.csv" PARENT_SCOPE)
    if(NOT IDF_TARGET)
        set(IDF_TARGET "esp32s3" PARENT_SCOPE)
        set(IDF_TARGET "esp32s3")
    endif()

    # Generated headers: the board description and the pinned store root key.
    set(FRAME_GEN_DIR "${CMAKE_BINARY_DIR}/frame_gen" PARENT_SCOPE)
    set(FRAME_GEN_DIR "${CMAKE_BINARY_DIR}/frame_gen")
    file(MAKE_DIRECTORY "${FRAME_GEN_DIR}")

    execute_process(
        COMMAND ${PYTHON} "${repo_root}/tools/gen_board_header.py"
                --device "${FRAME_BOARD_DIR}/device.json"
                --out    "${FRAME_GEN_DIR}/frame_board_gen.h"
        RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "gen_board_header.py failed for board '${BOARD}':\n${_err}")
    endif()

    # The store root public key is pinned into the firmware. Development builds
    # use the committed dev key; a --release build (tools/build.py) points this
    # at a key outside the repository and refuses keys/dev/.
    set(FRAME_STORE_KEY_FILE "${repo_root}/keys/dev/store_root.pub"
        CACHE FILEPATH "Ed25519 public key that signs the app catalog (*.pub)")
    execute_process(
        COMMAND ${PYTHON} "${repo_root}/tools/gen_store_key_header.py"
                --key "${FRAME_STORE_KEY_FILE}"
                --out "${FRAME_GEN_DIR}/frame_store_root_gen.h"
        RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "gen_store_key_header.py failed:\n${_err}")
    endif()

    # Board component plus every Frame component; names are fixed so callers
    # can REQUIRES frame_board regardless of the directory it lives in.
    # EXTRA_COMPONENT_DIRS can point to a single component directory or to a
    # container — IDF handles both. We also mirror to COMPONENT_DIRS for
    # IDF v5+ compatibility where EXTRA_COMPONENT_DIRS is deprecated.
    set(EXTRA_COMPONENT_DIRS "${FRAME_BOARD_DIR}" "${repo_root}/components" PARENT_SCOPE)
    set(COMPONENT_DIRS "${FRAME_BOARD_DIR}" "${repo_root}/components" PARENT_SCOPE)
    message(STATUS "FrameOS: BOARD=${BOARD} FRAME_BOARD_DIR=${FRAME_BOARD_DIR}")
    message(STATUS "FrameOS: EXTRA_COMPONENT_DIRS=${FRAME_BOARD_DIR};${repo_root}/components")

    # Release builds must pin a real store key and must not use the dev keypair
    # that lives in the repo. build.py enforces the sdkconfig checks (ALLOW_UNSIGNED
    # and HTTP_API); this catches the obvious mistake at configure time.
    if(FRAME_RELEASE)
        if(FRAME_STORE_KEY_FILE MATCHES "keys/dev")
            message(FATAL_ERROR "FRAME_RELEASE=1 but FRAME_STORE_KEY_FILE is still '${FRAME_STORE_KEY_FILE}' — a release must pin a key outside the repo (pass -DFRAME_STORE_KEY_FILE=/safe/place/store_root.pub or use tools/build.py --release --key <key>)")
        endif()
    endif()
endfunction()

# Call after project(): exposes the generated headers to every component.
function(frame_project_finish)
    # Use include_directories so the generated headers are visible without
    # going through the build-properties file (which would need quoting
    # gymnastics for the path).
    include_directories("${FRAME_GEN_DIR}")
endfunction()

# wasm3 interpreter (vendored, MIT — see wasm3/LICENSE) plus the Frame host
# API. The interpreter is compiled with warnings suppressed: it is third-party
# code and IDF v6 treats warnings as errors for our own sources only.
set(WASM3_DIR "${CMAKE_CURRENT_LIST_DIR}/wasm3/source")

set(WASM3_SRCS
    "${WASM3_DIR}/m3_bind.c"
    "${WASM3_DIR}/m3_code.c"
    "${WASM3_DIR}/m3_compile.c"
    "${WASM3_DIR}/m3_core.c"
    "${WASM3_DIR}/m3_emit.c"
    "${WASM3_DIR}/m3_env.c"
    "${WASM3_DIR}/m3_exec.c"
    "${WASM3_DIR}/m3_function.c"
    "${WASM3_DIR}/m3_info.c"
    "${WASM3_DIR}/m3_module.c"
    "${WASM3_DIR}/m3_optimize.c"
    "${WASM3_DIR}/m3_parse.c"
)

idf_component_register(
    SRCS
        "wasm.c"
        "ui_tree.c"
        ${WASM3_SRCS}
    INCLUDE_DIRS "include" "${WASM3_DIR}"
    REQUIRES frame_core frame_ui frame_storage frame_log lvgl
)

# Suppress warnings for vendored wasm3. This must be after
# idf_component_register and must not run in the component-requirements
# collection pass (cmake -P), where set_source_files_properties is not
# scriptable.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    set_source_files_properties(${WASM3_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
endif()

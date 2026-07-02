if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(
        REQUIRES
            esp_http_server
            tele_portal_core
    )
    return()
endif()

set(WEB_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../firmware_assets/web")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(GENERATED_ASSETS_C "${GENERATED_DIR}/tele_portal_assets_generated.c")
set(GENERATED_ASSETS_H "${GENERATED_DIR}/tele_portal_assets_generated.h")

file(MAKE_DIRECTORY "${GENERATED_DIR}")

file(GLOB_RECURSE WEB_ASSETS
    "${WEB_ASSETS_DIR}/*"
)

list(FILTER WEB_ASSETS EXCLUDE REGEX ".*/teste\.html$")

if(NOT CONFIG_TELE_PORTAL_LOGS_ENABLE_ENDPOINT)
    list(FILTER WEB_ASSETS EXCLUDE REGEX ".*/logs\.html$")
endif()

idf_build_get_property(PYTHON PYTHON)

add_custom_command(
    OUTPUT
        "${GENERATED_ASSETS_C}"
        "${GENERATED_ASSETS_H}"
    COMMAND
        ${PYTHON}
        "${CMAKE_CURRENT_LIST_DIR}/tools/gen_assets.py"
        --input-dir "${WEB_ASSETS_DIR}"
        --out-c "${GENERATED_ASSETS_C}"
        --out-h "${GENERATED_ASSETS_H}"
        ${WEB_ASSETS}
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/tools/gen_assets.py"
        ${WEB_ASSETS}
    VERBATIM
)

idf_component_register(
    SRCS
        "tele_portal_assets.c"
        "${GENERATED_ASSETS_C}"
    INCLUDE_DIRS
        "include"
        "${GENERATED_DIR}"
    EMBED_TXTFILES
        ${WEB_ASSETS}
    REQUIRES
        esp_http_server
        tele_portal_core
)
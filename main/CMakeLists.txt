# Built-in fixture profiles: every fixtures/*.json is bundled into one JSON array.
file(GLOB FSF_FIXTURE_FILES ${CMAKE_CURRENT_LIST_DIR}/../fixtures/*.json)
list(SORT FSF_FIXTURE_FILES)
set(FSF_BUILTIN_JSON "[")
set(sep "")
foreach(f ${FSF_FIXTURE_FILES})
    file(READ ${f} content)
    string(APPEND FSF_BUILTIN_JSON "${sep}${content}")
    set(sep ",")
endforeach()
string(APPEND FSF_BUILTIN_JSON "]")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/builtin_fixtures.json "${FSF_BUILTIN_JSON}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FSF_FIXTURE_FILES})

idf_component_register(
    SRCS
        main.c
        fsf_config.c
        fsf_engine.c
        fsf_fixtures.c
        fsf_io.c
        fsf_net.c
        fsf_rdm.c
        fsf_sacn.c
        fsf_storage.c
        fsf_web.c
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        www/index.html
        ../tools/profile-builder.html
        ${CMAKE_CURRENT_BINARY_DIR}/builtin_fixtures.json
    PRIV_REQUIRES
        esp_eth esp_netif esp_event esp_timer esp_http_server esp_adc esp_driver_rmt esp_driver_gpio
        nvs_flash json app_update esp_app_format lwip etc_rdmnet esp_system
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

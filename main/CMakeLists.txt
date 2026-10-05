idf_component_register(SRC_DIRS          "." "zigbee" "zigbee/handlers" "matter" "peripherals" "provisioning"
                       PRIV_INCLUDE_DIRS "." "zigbee" "zigbee/handlers" "matter" "peripherals" "provisioning" "${ESP_MATTER_PATH}/examples/common/utils")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-error=format=")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=loader_port_enter_bootloader -Wl,--wrap=loader_port_reset_target")

# ================================================================
#  构建元信息注入
#
#  将 git SHA、分支、版本注入编译定义，启动时打印。
#  当 git 不可用时自动回退到 build_info.h 中的默认值。
# ================================================================
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")

if(GIT_SHA)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "BUILD_GIT_SHA=\"${GIT_SHA}\"")
endif()
if(GIT_BRANCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "BUILD_GIT_BRANCH=\"${GIT_BRANCH}\"")
endif()
if(GIT_VERSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "BUILD_VERSION=\"${GIT_VERSION}\"")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE "BUILD_TIMESTAMP=\"${BUILD_TIMESTAMP}\"")

# Pack official ot_rcp (ESP32-H2) with the S3 app, then wrap a Matter OTA header.
# Overwrites zigbee_bridge-ota.bin produced by CHIP_OTA_IMAGE_BUILD.
if(CONFIG_GATEWAY_RCP_OTA)
    set(_RCP_DIR "$ENV{IDF_PATH}/examples/openthread/ot_rcp/build")
    if(EXISTS "${_RCP_DIR}/esp_ot_rcp.bin" AND EXISTS "${_RCP_DIR}/flash_args")
        idf_build_get_property(python PYTHON)
        set(_RCP_PACK_PY "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_rcp_update/create_ota_image.py")
        set(_OTA_TOOL "$ENV{ESP_MATTER_PATH}/connectedhomeip/connectedhomeip/src/app/ota_image_tool.py")
        add_custom_target(gateway_combined_ota ALL
            COMMAND ${python} ${_RCP_PACK_PY}
                --rcp-build-dir ${_RCP_DIR}
                --br-firmware ${CMAKE_BINARY_DIR}/zigbee_bridge.bin
                --target-file ${CMAKE_BINARY_DIR}/ota_with_rcp_image
            COMMAND ${python} ${_OTA_TOOL} create
                --vendor-id 0xfff1
                --product-id 0x8000
                --version ${PROJECT_VER_NUMBER}
                --version-str ${PROJECT_VER}
                --digest-algorithm sha256
                ${CMAKE_BINARY_DIR}/ota_with_rcp_image
                ${CMAKE_BINARY_DIR}/zigbee_bridge-s3h2-ota.bin
            DEPENDS app
            COMMENT "Matter OTA image = ESP32-S3 gateway + ESP32-H2 ot_rcp -> zigbee_bridge-s3h2-ota.bin"
            VERBATIM)
    else()
        message(WARNING "GATEWAY_RCP_OTA is on but ${_RCP_DIR} has no esp_ot_rcp.bin. "
                        "Build IDF examples/openthread/ot_rcp for esp32h2 first. "
                        "Matter OTA will be S3-only until then.")
    endif()
endif()
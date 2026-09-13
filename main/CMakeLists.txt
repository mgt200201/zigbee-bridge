idf_component_register(SRC_DIRS          "." "zigbee" "zigbee/handlers" "matter" "peripherals" "provisioning"
                       PRIV_INCLUDE_DIRS "." "zigbee" "zigbee/handlers" "matter" "peripherals" "provisioning" "${ESP_MATTER_PATH}/examples/common/utils")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-error=format=")

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
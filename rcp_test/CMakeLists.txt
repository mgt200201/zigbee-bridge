# RCP 测试项目 - 不依赖 Matter
# 复用主项目已下载的 managed_components，无需联网
cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS
    "/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__esp-zboss-lib"
    "/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__esp-zigbee-lib"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rcp_test)

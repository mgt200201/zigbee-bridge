idf_component_register(SRC_DIRS          "." "zigbee" "matter" "peripherals" "provisioning"
                       PRIV_INCLUDE_DIRS "." "zigbee" "matter" "peripherals" "provisioning" "${ESP_MATTER_PATH}/examples/common/utils")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-error=format=")
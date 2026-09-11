/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <esp_zigbee_core.h>
#include <stdint.h>
#include <stdbool.h>
#include <zcl/esp_zigbee_zcl_command.h>
#include <zcl/esp_zigbee_zcl_basic.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化设备发现模块
 */
void zdp_discovery_init(void);

/**
 * @brief 处理设备公告（由 zboss_manager 在收到 Device Annce 信号时调用）
 *
 * @param short_addr 设备短地址
 * @param ieee_addr 设备 IEEE 地址
 * @param steering_active 当前是否在入网窗口内
 */
void zdp_discovery_handle_device_annce(uint16_t short_addr,
                                       const esp_zb_ieee_addr_t ieee_addr,
                                       bool steering_active);

/**
 * @brief 获取 ZCL 动作处理器回调
 *
 * 由 zboss_manager 在初始化时注册到协议栈。
 *
 * @return esp_zb_core_action_callback_t
 */
esp_zb_core_action_callback_t zdp_discovery_get_action_handler(void);

/**
 * @brief 主动读取子设备的 Basic 集群属性
 *
 * 设备入网后，协调器主动向子设备发送 Read Attributes 请求，
 * 读取 Manufacturer Name、Model Identifier、SW Build ID、deviceId、uiid。
 *
 * @param short_addr 目标设备短地址
 * @param endpoint 目标设备端点号
 */
void zdp_discovery_read_basic_attributes(uint16_t short_addr, uint8_t endpoint);

#ifdef __cplusplus
}
#endif
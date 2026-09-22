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

/**
 * @brief 温湿度传感器入网后主动配置属性上报
 *
 * 当设备 uiid 识别为 UIID_TEMP_HUMIDITY_SENSOR (0x0001) 时调用，
 * 按主流温湿度计参数向子设备下发 Configure Reporting：
 *   - 温度 MeasuredValue (0x0402/0x0000)
 *   - 湿度 MeasuredValue (0x0405/0x0000)
 *
 * @param short_addr 目标设备短地址
 * @param endpoint   目标设备端点号
 */
void zdp_discovery_configure_reporting(uint16_t short_addr, uint8_t endpoint);

/**
 * @brief 通过私有集群 0xFC11 属性 0x1FFF 下发设备注册状态
 *
 * 用于协调器与子设备之间的注册流程握手机制：
 *   - 128: 协调器发现开始，设备等待
 *   - 129: Matter 注册完成，设备可继续操作
 *
 * @param short_addr 目标设备短地址
 * @param endpoint   目标设备端点号
 * @param status     状态值（128/129）
 */
void zdp_discovery_send_register_status(uint16_t short_addr, uint8_t endpoint, uint16_t status);

/**
 * @brief 处理设备离网事件
 *
 * 当收到 ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION 且 rejoin==0 时调用。
 * 负责清理 Matter 桥接设备、device_info 记录和 NVS 持久化数据。
 *
 * @param short_addr 离网设备短地址
 */
void zdp_discovery_handle_device_leave(uint16_t short_addr);

#ifdef __cplusplus
}
#endif
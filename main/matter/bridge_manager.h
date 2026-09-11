/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include <app_bridged_device.h>
#include <esp_zigbee_core.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Matter 设备类型 ID - 温湿度传感器
 */
#define ESP_MATTER_TEMP_HUMIDITY_SENSOR_DEVICE_TYPE_ID 0x0310

/* ==================== 生命周期 ==================== */

/**
 * @brief 初始化桥接管理器
 * @param aggregator_endpoint_id Aggregator 端点 ID
 */
void bridge_manager_init(uint16_t aggregator_endpoint_id);

/* ==================== 回调函数（供 app_main 注册到 Matter 栈） ==================== */

/**
 * @brief Matter/Chip 事件回调
 *
 * 处理 WiFi 连接、Commissioning、配对码打印等事件。
 * 注册方式: esp_matter::start(bridge_manager_event_cb);
 */
void bridge_manager_event_cb(const ChipDeviceEvent *event, intptr_t arg);

/**
 * @brief 桥接设备属性更新回调
 *
 * 当 Matter 控制器向桥接设备写入属性时调用。
 * 注册方式: node::create(&config, bridge_manager_attr_update_cb, NULL);
 */
esp_err_t bridge_manager_attr_update_cb(esp_matter::attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data);

/**
 * @brief 桥接端点创建回调
 *
 * 创建桥接设备时调用，用于添加数据模型元素。
 * 注册方式: app_bridge_initialize(node, bridge_manager_create_devices_cb);
 */
esp_err_t bridge_manager_create_devices_cb(esp_matter::endpoint_t *ep,
                                            uint32_t device_type_id, void *priv_data);

/* ==================== 桥接设备管理 ==================== */

bool bridge_manager_is_device_bridged(uint16_t short_addr);
uint8_t bridge_manager_get_endpoint_by_short(uint16_t short_addr);

/**
 * @brief 设备发现完成回调（由 zdp_discovery 在 Simple Desc 响应时调用）
 */
void bridge_manager_on_device_discovered(uint16_t short_addr, uint8_t endpoint);

/**
 * @brief 设备标识完成回调（由 zdp_discovery 在 Read Attr 响应时调用）
 */
void bridge_manager_on_device_identified(uint32_t device_id, const char *uiid,
                                          uint16_t short_addr, uint8_t endpoint,
                                          const esp_zb_ieee_addr_t ieee_addr);

/**
 * @brief 上报传感器数据到 Matter
 */
void bridge_manager_report_sensor_data(uint16_t short_addr, uint8_t endpoint,
                                       uint16_t cluster_id, int16_t raw_value);

/**
 * @brief 处理来自 Matter 侧的桥接设备属性更新请求
 */
esp_err_t bridge_manager_attr_update(uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id,
                                      esp_matter_attr_val_t *val,
                                      app_bridged_device_t *zigbee_device);

#ifdef __cplusplus
}
#endif
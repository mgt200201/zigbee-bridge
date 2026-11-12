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
#include "device_info.h"
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

/**
 * @brief 设置 Zigbee 栈是否已就绪。
 *
 * 由 zboss_manager 在 esp_zb_init() 完成后调用。栈未就绪前禁止任何下行 Zigbee 命令，
 * 否则开机 resume 端点时属性 init 回调会触发 esp_zb_zcl_*_cmd_req()，
 * 而 ZBOSS 的 bufpool 尚未分配，直接访问空指针崩溃。
 */
void bridge_manager_set_zb_stack_ready(bool ready);

bool bridge_manager_is_device_bridged(uint16_t short_addr);
uint8_t bridge_manager_get_endpoint_by_short(uint16_t short_addr);

/**
 * @brief 设备发现完成回调（由 zdp_discovery 在 Simple Desc 响应时调用）
 */
void bridge_manager_on_device_discovered(uint16_t short_addr, uint8_t endpoint);

/**
 * @brief 设备标识完成回调（由 zdp_discovery 在 Read Attr 响应时调用）
 *
 * @param identity Basic 集群读到的厂商/型号/版本信息，可为 NULL。
 *                 信息会写入桥接端点的 Bridged Device Basic Information 集群。
 */
void bridge_manager_on_device_identified(uint32_t device_id, uint16_t uiid,
                                          uint16_t short_addr, uint8_t endpoint,
                                          const esp_zb_ieee_addr_t ieee_addr,
                                          const device_identity_t *identity);

/**
 * @brief 用 device_info 中持久化的标识信息回填所有桥接端点属性
 *
 * 网关重启后桥接端点从 NVS 恢复，但 ProductName/NodeLabel 等属性不会自动恢复，
 * 会导致 HA 里设备名变成默认值。在 app_bridge_initialize() 之后调用一次即可。
 */
void bridge_manager_restore_identities(void);

/**
 * @brief 清理没有对应 Zigbee 设备的残留桥接端点
 *
 * 子设备离网时若端点销毁失败（或历史版本遗留），Matter 数据模型里会留下孤儿端点，
 * HA 会一直显示该设备。启动时在 bridge_manager_restore_identities() 之后调用一次，
 * 把没有 device_info 记录的桥接端点全部销毁。
 */
void bridge_manager_reconcile_bridged_devices(void);

/**
 * @brief 延迟触发一次根端点 Descriptor.PartsList 变化报告
 *
 * 桥接端点销毁后，esp_matter 在 disable() 里标记的 PartsList 脏标记经常与
 * 正在生成的订阅报告重叠而被丢弃，HA 只收到删除前的列表，实体长期残留。
 * 销毁完成后调用本函数，延迟一小段时间再显式上报一次，此时端点已从数据模型
 * 中移除，HA 收到的 PartsList 一定不含它。
 */
void bridge_manager_notify_parts_list_changed(void);

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
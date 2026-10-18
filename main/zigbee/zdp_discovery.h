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
#include <zdo/esp_zigbee_zdo_command.h>

#ifdef __cplusplus
extern "C" {
#endif

void zdp_discovery_on_simple_desc(esp_zb_zdp_status_t zdo_status,
                                  esp_zb_af_simple_desc_1_1_t *simple_desc,
                                  void *user_ctx);

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
 * @brief 读取彩色灯的 ColorMode/EnhancedColorMode（0x0300 集群 0x0008/0x4001）
 *
 * 子设备受 ZBOSS 限制无法主动上报 ColorMode，只能由网关读取校准：
 * 结果经 color handler 的读属性响应回调写入 Matter 端。
 *
 * @param short_addr 子设备短地址
 * @param endpoint   子设备端点号
 */
void zdp_discovery_read_color_mode(uint16_t short_addr, uint8_t endpoint);

/**
 * @brief 读取子设备当前运行的固件版本（OTA 集群 0x0019 / 0x0002）
 *
 * 入网与 rejoin 时各读一次，结果由 ota handler 的读属性响应回调打印。
 * 子设备若未实现 OTA client 集群，会返回 UNSUPPORTED_CLUSTER，属正常情况。
 *
 * @param short_addr 子设备短地址
 * @param endpoint   子设备端点号
 */
void zdp_discovery_read_ota_version(uint16_t short_addr, uint8_t endpoint);

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
 * @brief 彩色灯 (uiid 0x0002) 入网后主动配置属性上报
 *
 * 向子设备下发 Configure Reporting，使其主动上报：
 *   - OnOff 开关状态
 *   - Level 亮度 CurrentLevel
 *   - ColorControl 色彩 (Hue/Sat/X/Y/ColorMode) 与色温 (ColorTemperatureMireds)
 *
 * @param short_addr 目标设备短地址
 * @param endpoint   目标设备端点号
 */
void zdp_discovery_configure_color_reporting(uint16_t short_addr, uint8_t endpoint);

/**
 * @brief 将彩灯 (uiid 0x0002) 的 OnOff/Level/ColorControl 集群绑定到网关端点
 *
 * Configure Reporting 需要配合绑定，报告才有目的地址；仅首次配网时下发。
 *
 * @param short_addr 子设备短地址（请求发往地址）
 * @param endpoint   子设备上的源端点
 * @param src_ieee   子设备的 IEEE 地址（绑定记录里的源地址）
 */
void zdp_discovery_bind_light(uint16_t short_addr, uint8_t endpoint, esp_zb_ieee_addr_t src_ieee);

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
 * @brief 触发子设备主动上报当前状态（OnOff/Level/Color）
 *
 * 向子设备的 Basic 集群(0x0000)属性 0x1FFF 写入 U8 值 129。
 * 子设备收到后会把 NVS 中保存的真实状态写回 ZCL 并主动上报一次，
 * 用于在网关(重新)为设备创建 Matter 端点后，第一时间把设备真实状态
 * 同步到 Matter，避免"重配后第一次控制无效"。
 *
 * 注意：这是子设备约定的触发点，必须写 Basic(0x0000)/0x1FFF 且类型为 U8，
 * 与旧的 0xFC11 私有集群握手（zdp_discovery_send_register_status）不同。
 *
 * @param short_addr 目标设备短地址
 * @param endpoint   目标设备端点号
 */
void zdp_discovery_request_state_report(uint16_t short_addr, uint8_t endpoint);

/**
 * @brief 处理设备离网事件
 *
 * 当收到 ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION 且 rejoin==0 时调用。
 * 负责清理 Matter 桥接设备、device_info 记录和 NVS 持久化数据。
 *
 * 短地址（short_addr）会随重加入网而变化，因此必须同时传入 IEEE 地址。
 * 删除 device_info 时优先按稳定的 IEEE 查找，避免因短地址过期导致
 * device_info 残留、进而使下一次重入网误走"重连"路径而不重建 Matter 端点。
 *
 * @param short_addr 离网设备短地址
 * @param ieee 离网设备 IEEE 64 位地址（唯一且稳定）
 */
void zdp_discovery_handle_device_leave(uint16_t short_addr, const esp_zb_ieee_addr_t ieee);

#ifdef __cplusplus
}
#endif
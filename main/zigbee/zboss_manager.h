/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zboss_manager.h
 *
 * @brief Zigbee 管理层统一入口（Facade）
 *
 * 本文件是 Zigbee 管理层的公共 API 入口。
 * 具体实现已按单一职责拆分为子模块：
 *   - zigbee_network.h/.cpp  → 网络状态 + Steering 管理
 *   - zigbee_device_db.h/.cpp → 已知设备数据库 + 端点映射
 *
 * 本文件保留：
 *   - 配置宏 (HA_GW_ENDPOINT, ESP_ZB_ZC_CONFIG 等)
 *   - 生命周期 API (zboss_manager_start)
 *   - 信号处理入口 (esp_zb_app_signal_handler)
 *   - 向后兼容的 thin wrapper
 */

#pragma once

#include <esp_err.h>
#include <esp_zigbee_core.h>
#include <stdint.h>
#include <stdbool.h>

#include "zigbee_network.h"
#include "zigbee_device_db.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 配置参数 ==================== */

#define HA_GW_ENDPOINT          1
#define MAX_CHILDREN            10
#define INSTALLCODE_POLICY_ENABLE false
#define ESP_ZB_PRIMARY_CHANNEL_MASK (1l << 13)

#define ESP_ZB_ZC_CONFIG()                                      \
    {                                                           \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,          \
        .install_code_policy = INSTALLCODE_POLICY_ENABLE,       \
        .nwk_cfg = {                                            \
            .zczr_cfg = { .max_children = MAX_CHILDREN },       \
        },                                                      \
    }

/* ==================== 向后兼容类型别名 ==================== */

#define MAX_KNOWN_DEVICES ZIGBEE_DB_MAX_DEVICES
typedef zigbee_db_device_t known_device_t;
typedef zigbee_net_steering_cb_t steering_state_callback_t;

/* ==================== 生命周期 ==================== */

void zboss_manager_start(void);
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct);

/**
 * Pause the Zigbee task and uninstall UART1 so serial-flasher can own the
 * H2 UART for RCP firmware download. The gateway reboots after Matter OTA,
 * so this is not resumed in place.
 */
esp_err_t zboss_manager_pause_for_rcp_flash(void);

/* ==================== 网络管理（thin wrapper → zigbee_network） ==================== */

static inline bool zboss_manager_is_network_formed(void)
{
    return zigbee_net_is_formed();
}

static inline bool zboss_manager_is_steering_active(void)
{
    return zigbee_net_is_steering();
}

static inline void zboss_manager_start_steering(void)
{
    zigbee_net_start_steering();
}

static inline void zboss_manager_stop_steering(void)
{
    zigbee_net_stop_steering();
}

static inline bool zboss_manager_steering_is_expired(void)
{
    return zigbee_net_steering_expired();
}

static inline void zboss_manager_register_steering_callback(steering_state_callback_t cb)
{
    zigbee_net_register_steering_cb(cb);
}

static inline void zboss_manager_handle_short_press(void)
{
    zigbee_net_handle_button();
}

/* ==================== 设备管理（thin wrapper → zigbee_device_db） ==================== */

static inline known_device_t *zboss_manager_get_known_devices(void)
{
    return zigbee_db_get_all();
}

static inline known_device_t *zboss_manager_find_by_ieee(const esp_zb_ieee_addr_t ieee)
{
    return zigbee_db_find_by_ieee(ieee);
}

static inline known_device_t *zboss_manager_find_by_short(uint16_t short_addr)
{
    return zigbee_db_find_by_short(short_addr);
}

/* ==================== 端点映射（thin wrapper → zigbee_device_db） ==================== */

static inline void zboss_manager_register_bridged_device(uint16_t short_addr, uint8_t ep)
{
    zigbee_db_register_ep(short_addr, ep);
}

static inline uint16_t zboss_manager_find_short_addr_by_ep(uint8_t ep)
{
    return zigbee_db_short_by_ep(ep);
}

static inline uint8_t zboss_manager_find_ep_by_short(uint16_t short_addr)
{
    return zigbee_db_ep_by_short(short_addr);
}

#ifdef __cplusplus
}
#endif
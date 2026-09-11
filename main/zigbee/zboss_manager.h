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

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 配置参数 ==================== */

/** 网关 Zigbee endpoint，用于接收子设备报告 */
#define HA_GW_ENDPOINT          1
/** 最大子设备数量 */
#define MAX_CHILDREN            10
/** 安装码策略开关 */
#define INSTALLCODE_POLICY_ENABLE false
/** Zigbee 主信道掩码 */
#define ESP_ZB_PRIMARY_CHANNEL_MASK (1l << 13)

/**
 * @brief Zigbee 协调器配置宏
 */
#define ESP_ZB_ZC_CONFIG()                                      \
    {                                                           \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,          \
        .install_code_policy = INSTALLCODE_POLICY_ENABLE,       \
        .nwk_cfg = {                                            \
            .zczr_cfg = { .max_children = MAX_CHILDREN },       \
        },                                                      \
    }

/* ==================== 生命周期 ==================== */

/**
 * @brief 启动 Zigbee BOSS 服务
 *
 * 初始化 Zigbee 平台配置，创建 zboss_task，启动协议栈。
 * 此函数不会阻塞，协议栈在其自己的 FreeRTOS 任务中运行。
 */
void zboss_manager_start(void);

/**
 * @brief Zigbee 协议栈信号处理函数
 *
 * 由 ZBOSS 回调，处理网络组建、设备入网/离网等信号。
 */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct);

/* ==================== 网络管理 ==================== */

/** @return true 网络已组建完成 */
bool zboss_manager_is_network_formed(void);

/** @return true 入网窗口已开放 */
bool zboss_manager_is_steering_active(void);

/**
 * @brief 开放入网窗口
 *
 * 通过 ZBOSS scheduler 发起 BDB Network Steering。
 * 窗口持续 STEERING_DURATION_SEC 秒后自动关闭。
 */
void zboss_manager_start_steering(void);

/**
 * @brief 立即关闭入网窗口
 */
void zboss_manager_stop_steering(void);

/* ==================== 已知设备管理 ==================== */

/** 最大缓存的已知设备数量 */
#define MAX_KNOWN_DEVICES 16

/**
 * @brief 已知设备结构体
 */
typedef struct {
    bool in_use;
    esp_zb_ieee_addr_t ieee_addr;
    uint16_t short_addr;
} known_device_t;

/** @return 已知设备列表指针 */
known_device_t *zboss_manager_get_known_devices(void);

/**
 * @brief 根据 IEEE 地址查找已知设备
 */
known_device_t *zboss_manager_find_by_ieee(const esp_zb_ieee_addr_t ieee);

/**
 * @brief 根据短地址查找已知设备
 */
known_device_t *zboss_manager_find_by_short(uint16_t short_addr);

/* ==================== 桥接端点映射 ==================== */

/**
 * @brief 注册桥接设备到 Zigbee BOSS
 *
 * 将 Zigbee 设备（短地址+端点）注册到内部映射表中，
 * 使后续的 ZCL 报告能够正确路由到对应的 Matter 桥接设备。
 *
 * @param short_addr Zigbee 设备短地址
 * @param zigbee_endpoint Zigbee 设备端点号
 */
void zboss_manager_register_bridged_device(uint16_t short_addr, uint8_t zigbee_endpoint);

/**
 * @brief 根据 Zigbee 端点号查找对应的短地址
 * @param zigbee_endpoint Zigbee 端点号
 * @return 找到返回短地址，未找到返回 0xFFFF
 */
uint16_t zboss_manager_find_short_addr_by_ep(uint8_t zigbee_endpoint);

/**
 * @brief 根据短地址查找对应的 Zigbee 端点号
 * @param short_addr Zigbee 短地址
 * @return 找到返回端点号，未找到返回 0xFF
 */
uint8_t zboss_manager_find_ep_by_short(uint16_t short_addr);

/* ==================== Steering 状态回调（供 LED 反馈等） ==================== */

/**
 * @brief Steering 状态变化回调类型
 * @param active true=入网窗口开放中, false=已关闭
 */
typedef void (*steering_state_callback_t)(bool active);

/**
 * @brief 注册 Steering 状态变化回调
 *
 * 用于 LED 闪烁等外部反馈。当 zboss_manager 内部状态变化时自动调用。
 *
 * @param cb 回调函数指针，传入 NULL 可取消注册
 */
void zboss_manager_register_steering_callback(steering_state_callback_t cb);

/**
 * @brief 检查 Steering 是否已超时
 * @return true 已超时
 */
bool zboss_manager_steering_is_expired(void);

/* ==================== 按键处理（由 app_main 路由） ==================== */

/**
 * @brief 处理按键短按事件
 *
 * 切换 Steering 入网窗口的开启/关闭。
 * 当网络未组建时无操作。
 */
void zboss_manager_handle_short_press(void);

#ifdef __cplusplus
}
#endif
/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @brief ZCL 消息路由核心
 *
 * 所有集群按 cluster_id 注册到 Handler 注册表。
 * 收到属性报告 → 按 cluster_id 匹配 handler → 未命中则记录 metrics。
 *
 * 新增 cluster = 实现 zcl_device_handler_t → zcl_handlers_init() 注册一行。
 */

#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <esp_zigbee_core.h>
#include <zcl/esp_zigbee_zcl_command.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 自定义 Handler 接口 ==================== */

/** 属性报告回调 */
typedef esp_err_t (*zcl_report_attr_handler_t)(const esp_zb_zcl_report_attr_message_t *message);

/** 属性写入（Matter → Zigbee）回调 */
typedef esp_err_t (*zcl_set_attr_handler_t)(const esp_zb_zcl_set_attr_value_message_t *message);

/** 读取属性响应回调 */
typedef esp_err_t (*zcl_read_attr_resp_handler_t)(const esp_zb_zcl_cmd_read_attr_resp_message_t *message);

/**
 * @brief ZCL 设备处理器
 *
 * 每个自定义设备类型对应一个静态实例。
 * 简单传感器不用创建此结构体——由传感器路由表自动处理。
 */
typedef struct {
    uint16_t cluster_id;                        /**< Zigbee cluster ID */
    const char *name;                           /**< 处理器名称（仅用于日志） */
    zcl_report_attr_handler_t on_report_attr;   /**< 属性报告回调 */
    zcl_set_attr_handler_t on_set_attr;         /**< 属性写入回调 */
    zcl_read_attr_resp_handler_t on_read_attr_resp; /**< 读取响应回调 */
} zcl_device_handler_t;

/* ==================== API ==================== */

/**
 * @brief 注册一个自定义设备处理器
 *
 * 仅用于需要特殊处理逻辑的集群（On/Off、私有协议等）。
 * 简单传感器不需要调用此函数——由传感器路由表自动处理。
 *
 * @param handler 处理器指针（必须指向静态/全局变量）
 * @return ESP_OK 注册成功
 */
esp_err_t zcl_handler_register(const zcl_device_handler_t *handler);

/**
 * @brief 全局 ZCL 消息分发器
 *
 * 路由顺序：
 *   1. 传感器路由表（自动捕获已知传感器 cluster）
 *   2. Handler 注册表（按 cluster_id 匹配自定义 handler）
 *   3. 返回 ESP_ERR_NOT_FOUND 让调用方 fallback
 *
 * 内部集成 metrics 统计。
 *
 * @param callback_id 回调类型
 * @param message     消息指针
 * @return ESP_OK / ESP_ERR_NOT_FOUND
 */
esp_err_t zcl_handler_dispatch(esp_zb_core_action_callback_id_t callback_id, const void *message);

/**
 * @brief 初始化所有内置处理逻辑
 *
 * 注册传感器路由表 + 注册自定义 handler。
 * 新增传感器 = 在 sensor_clusters[] 数组加一行。
 * 新增自定义设备 = 加一行 zcl_handler_register()。
 */
void zcl_handlers_init(void);

/**
 * @brief 获取已注册的自定义 handler 数量
 */
int zcl_handler_count(void);

/**
 * @brief 从 ZCL 消息中获取设备短地址（工具函数）
 */
uint16_t zcl_get_short_addr(uint8_t src_endpoint, const esp_zb_zcl_addr_t *src_address);

#ifdef __cplusplus
}
#endif
/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file commissioning_mgr.h
 *
 * @brief Matter 配网凭据管理
 *
 * 管理 PIN 码和 Discriminator 的生成、持久化和读取。
 *
 * 安全要求（大厂标准）：
 *   - PIN 码首次启动时随机生成，持久化到 NVS
 *   - 后续启动从 NVS 读取，保证一致性
 *   - 设备出厂时打印在标签上（由产测工具读取 NVS 输出）
 *
 * 本模块不依赖 Matter SDK，使用原始 NVS API。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 配网凭据 ── */
#define COMMISSIONING_PIN_MIN  10000000   /* 8 位 PIN 最小值 */
#define COMMISSIONING_PIN_MAX  99999998   /* 8 位 PIN 最大值 */
#define COMMISSIONING_PIN_LEN  8

/**
 * @brief 初始化配网凭据
 *
 * 首次启动：生成随机 PIN + discriminator，写入 NVS
 * 后续启动：从 NVS 读取
 *
 * 必须在 NVS 初始化后调用。
 *
 * @return ESP_OK 成功
 */
esp_err_t commissioning_mgr_init(void);

/**
 * @brief 获取当前配网 PIN 码
 * @return 8 位 PIN 码（10000000-99999998）
 */
uint32_t commissioning_mgr_get_pin(void);

/**
 * @brief 获取当前 Discriminator
 * @return 12-bit discriminator（0-4095）
 */
uint16_t commissioning_mgr_get_discriminator(void);

/**
 * @brief 重新生成配网凭据（Factory Reset 时调用）
 * @return ESP_OK 成功
 */
esp_err_t commissioning_mgr_regenerate(void);

#ifdef __cplusplus
}
#endif
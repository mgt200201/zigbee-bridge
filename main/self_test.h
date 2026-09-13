/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file self_test.h
 *
 * @brief 启动自检模块
 *
 * 大厂标准实践：每个固件启动时必须执行自检，包括：
 *   1. 硬件自检 — 检测外设 (LED、按键) 是否正常
 *   2. 配置验证 — Kconfig 参数自洽性检查
 *   3. 模块健康检查 — 各模块初始化后回检
 *
 * 自检失败不阻止启动（嵌入式设备可能无法现场维修），
 * 但会打印 WARN 日志并返回错误码，供上层决策降级策略。
 */

#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 运行所有启动自检项目
 *
 * 调用顺序：
 *   1. self_test_hardware()  — 外设检测
 *   2. self_test_config()    — Kconfig 参数验证
 *
 * @return ESP_OK 全部通过
 * @return ESP_ERR_INVALID_STATE 部分项目失败（日志会打印详情）
 */
esp_err_t self_test_run(void);

/**
 * @brief 硬件自检
 *
 * 检测内容：
 *   - LED GPIO 是否可以正常拉高/拉低
 *   未来可扩展：按键中断、I2C 扫描等
 *
 * @return ESP_OK 硬件正常
 */
esp_err_t self_test_hardware(void);

/**
 * @brief Kconfig 配置自洽性验证
 *
 * 检测内容：
 *   - ANNCE_DELAY < DISCOVERY_RETRY_BASE（首次探测应在重试前完成）
 *   - MAX_HANDLERS >= 8（至少容纳基本设备类型）
 *   - MAX_ENDPOINT_PROBE 在合理范围内
 *
 * @return ESP_OK 配置通过
 * @return ESP_ERR_INVALID_ARG 配置冲突
 */
esp_err_t self_test_config(void);

#ifdef __cplusplus
}
#endif
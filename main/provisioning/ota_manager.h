/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file ota_manager.h
 *
 * @brief OTA 固件升级管理
 *
 * 提供固件版本检查、HTTPS OTA 下载、校验和回滚功能。
 *
 * 架构：
 *   - ota_manager_init()  → 初始化（含证书配置、事件回调注册）
 *   - ota_manager_check() → 检查服务端是否有新版本
 *   - ota_manager_upgrade() → 执行 OTA 下载 + 重启
 *
 * 依赖 Kconfig：
 *   - CONFIG_OTA_FW_UPGRADE_URL  →  固件升级 URL
 *   - CONFIG_OTA_AUTO_CHECK_HOURS →  自动检查间隔（0=禁用）
 *   - CONFIG_OTA_HTTPS_CERT_PEM   →  HTTPS 证书（可选）
 */

#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 HTTPS OTA 管理器（开发备用路径）
 *
 * 网关自身的正式升级走 Matter OTA Requestor（见 matter_ota.h）。
 * 本模块只从 CONFIG_OTA_FW_UPGRADE_URL 拉普通 bin，不走 Matter。
 * 在 WiFi 可用之后调用。自动检查默认关闭，避免和 Matter OTA 抢分区。
 *
 * @return ESP_OK 成功，其它值失败
 */
esp_err_t ota_manager_init(void);

/**
 * @brief 检查是否有新固件
 *
 * 向 CONFIG_OTA_FW_UPGRADE_URL 发起 HTTP HEAD/GET 请求，
 * 对比服务端版本号与本地版本。
 *
 * @param new_version_available 输出：true=有新版本
 * @param server_version        输出：服务端版本字符串（需调用者释放）
 * @return ESP_OK 检查成功（即使无新版本），其它值表示网络错误
 */
esp_err_t ota_manager_check(bool *new_version_available, char **server_version);

/**
 * @brief 执行 OTA 固件下载并重启
 *
 * 从 CONFIG_OTA_FW_UPGRADE_URL 下载固件，写入 OTA 分区，
 * 校验后标记为启动分区并重启。
 *
 * @warning 此函数会重启设备，永不返回 ESP_OK。
 *          失败时返回错误码（固件未写入）。
 *
 * @return 失败时返回错误码，成功时不返回（设备重启）
 */
esp_err_t ota_manager_upgrade(void);

/**
 * @brief 启动周期性 OTA 检查（后台任务）
 *
 * 按 CONFIG_OTA_AUTO_CHECK_HOURS 间隔自动检查。
 * 若该值为 0，本函数无操作。
 */
void ota_manager_start_periodic_check(void);

#ifdef __cplusplus
}
#endif
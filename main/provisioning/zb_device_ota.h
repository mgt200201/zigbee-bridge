/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zb_device_ota.h
 *
 * @brief Zigbee 子设备 OTA：HA 经 HTTP 上传镜像 → 网关作 OTA Server 下发
 *
 * 可靠路径（推荐）：
 *   PUT http://<gw-ip>:8088/api/zb_ota/upgrade?ieee=AA:BB:CC:DD:EE:FF:11:22
 *   Body = Zigbee OTA 文件（image_builder_tool 生成，含 0x0BEEF11E 头）
 *
 * 分步：
 *   PUT  /api/zb_ota/image
 *   POST /api/zb_ota/start?ieee=...  或  ?short=0xNNNN
 *   GET  /api/zb_ota/status
 */

#pragma once

#include <esp_err.h>
#include <esp_zigbee_core.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZB_DEVICE_OTA_HTTP_PORT  8088

typedef enum {
    ZB_DEVICE_OTA_STATE_IDLE = 0,
    ZB_DEVICE_OTA_STATE_STAGED,
    ZB_DEVICE_OTA_STATE_NOTIFYING,
    ZB_DEVICE_OTA_STATE_TRANSFERRING,
    ZB_DEVICE_OTA_STATE_DONE,
    ZB_DEVICE_OTA_STATE_FAILED,
} zb_device_ota_state_t;

/** 查找 zb_ota 分区；不启动 HTTP（等 WiFi IP）。 */
esp_err_t zb_device_ota_init(void);

/**
 * 往协调器 cluster_list 挂 OTA Upgrade Server（0x0019）。
 * 须在 esp_zb_device_register() 之前调用。
 */
esp_err_t zb_device_ota_add_server_cluster(esp_zb_cluster_list_t *cluster_list);

/** WiFi 拿到 IP 后启动本地 HTTP（可重复调用，幂等）。 */
esp_err_t zb_device_ota_http_start(void);

/** 供 zdp_discovery action handler 转发 OTA Server 回调。 */
esp_err_t zb_device_ota_handle_action(esp_zb_core_action_callback_id_t callback_id,
                                     const void *message);

zb_device_ota_state_t zb_device_ota_get_state(void);

#ifdef __cplusplus
}
#endif

/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file matter_ota.h
 *
 * @brief 网关自身的 Matter OTA Requestor
 *
 * 升级对象是网关自己（ESP32-S3）以及 UART 上的官方 ot_rcp（ESP32-H2）。
 * 双芯片发布物是 build/zigbee_bridge-s3h2-ota.bin：Matter 头 + 组合镜像
 * （H2 bootloader/分区表/app + S3 app）。仅升 S3 仍可用 zigbee_bridge-ota.bin。
 * 下载时 S3 暂停 Zigbee UART，用 serial-flasher 刷 H2，再把 S3 镜像写入未运行
 * 的 ota 槽；新 S3 固件启动确认后才 mark valid。
 *
 * H2 OTA：S3 GPIO(PIN_TO_RCP_BOOT，默认 IO3)→H2 BOOT，
 * S3 GPIO(PIN_TO_RCP_RESET，默认 IO46)→H2 EN。平时两脚拉高让 H2 跑固件；
 * OTA 时 serial-flasher 拉低 BOOT 并脉冲 EN 进下载。
 * 先编译 $IDF_PATH/examples/openthread/ot_rcp（esp32h2 + NCP_VENDOR_HOOK）。
 */

#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册可续传的 Matter OTA image processor
 *
 * @return ESP_OK 成功。Requestor 未打开时返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t matter_ota_init(void);

/**
 * @brief 把当前编译版本写入 NVS，并与上次上电记录对比
 *
 * 在 esp_matter::start() 之后调用。日志会标 first / upgraded / rolled back / unchanged。
 */
void matter_ota_persist_boot_version(void);

#ifdef __cplusplus
}
#endif

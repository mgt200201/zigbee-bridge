/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_ota.h
 *
 * @brief OTA Upgrade cluster handler (0x0019)
 *
 * 仅用于读取子设备当前运行的固件版本（CurrentFileVersion）；
 * OTA 服务端功能由 zb_device_ota 实现，与本 handler 无关。
 */

#pragma once

#include "zcl_handler_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const zcl_device_handler_t ota_upgrade_handler;

#ifdef __cplusplus
}
#endif

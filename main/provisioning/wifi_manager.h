/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WiFi 管理层
 *
 * 注册 WiFi 事件回调，初始化重连策略。
 */
void wifi_manager_init(void);

/**
 * @brief 处理 Chip 设备事件中的 WiFi/网络相关事件
 *
 * 由 bridge_manager_event_cb 在收到 Chip DeviceEvent 时调用，
 * 处理 kInterfaceIpAddressChanged、kWiFiConnectivityChange 等事件。
 *
 * @param event Chip 设备事件指针（实际类型为 const ChipDeviceEvent *）
 * @param arg 用户参数（透传）
 */
void wifi_manager_handle_chip_event(const void *event, intptr_t arg);

#ifdef __cplusplus
}
#endif
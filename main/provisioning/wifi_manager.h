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

/** 默认时区，POSIX TZ 格式。东八区为 "CST-8"，东九区 "JST-9"，UTC 为 "UTC0" */
#define WIFI_MANAGER_TZ_DEFAULT   "CST-8"

/**
 * @brief 初始化 WiFi 管理层
 *
 * 注册 WiFi 事件回调，初始化重连策略。
 */
void wifi_manager_init(void);

/**
 * @brief 设置时区（运行时可变）
 *
 * 采用 POSIX TZ 格式，例如 "CST-8"（UTC+8）、"JST-9"（UTC+9）、
 * "UTC0"、"EST5EDT" 等。修改后系统本地时间与 RTC 写入的本地时间同步更新。
 *
 * @param tz POSIX 时区字符串，NULL 或空串则忽略
 */
void wifi_manager_set_timezone(const char *tz);

/**
 * @brief 获取当前时区字符串
 */
const char *wifi_manager_get_timezone(void);

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
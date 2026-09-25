/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zcl_bridge_api.cpp
 *
 * @brief 桥接层抽象接口（依赖注入）
 *
 * 全局单例存储，ZCL handler 层不关心底层实现是 Matter 还是其他协议。
 */

#include "zcl_bridge_api.h"
#include <esp_log.h>
#include <stddef.h>

static const char *TAG = "zcl_bridge_api";

/** 全局桥接接口指针 */
static const zcl_bridge_api_t *s_bridge_api = NULL;

void zcl_bridge_api_register(const zcl_bridge_api_t *api)
{
    if (!api) {
        ESP_LOGE(TAG, "NULL bridge API rejected");
        return;
    }
    s_bridge_api = api;
    ESP_LOGI(TAG, "Bridge API registered (%s%s%s%s%s%s)",
             api->report_sensor ? "report_sensor " : "",
             api->report_on_off ? "report_on_off " : "",
             api->report_level ? "report_level " : "",
             api->report_color_xy ? "report_color_xy " : "",
             api->report_color_hsv ? "report_color_hsv " : "",
             api->is_device_bridged ? "is_device_bridged " : "");
}

const zcl_bridge_api_t *zcl_bridge_api_get(void)
{
    return s_bridge_api;
}
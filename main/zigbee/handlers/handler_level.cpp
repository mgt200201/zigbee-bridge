/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_level.cpp
 *
 * @brief Level Control cluster (0x0008) handler 实现
 *
 * 负责将 Zigbee 亮度属性上报 (CurrentLevel) 转发到 Matter 桥接层。
 */

#include "handler_level.h"
#include "zcl_bridge_api.h"
#include "zcl_metrics.h"
#include <esp_log.h>
#include <ha/esp_zigbee_ha_standard.h>

static const char *TAG = "level";

static esp_err_t level_report_attr(const esp_zb_zcl_report_attr_message_t *msg)
{
    uint16_t short_addr = zcl_get_short_addr(msg->src_endpoint, &msg->src_address);
    if (short_addr == 0xFFFF) {
        return ESP_ERR_NOT_FOUND;
    }

    switch (msg->attribute.id) {

    case ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID:
        if (msg->attribute.data.size >= 1) {
            uint8_t level = *(uint8_t *)msg->attribute.data.value;
            ESP_LOGI(TAG, "0x%04x ep=%d: level=%u", short_addr, msg->src_endpoint, level);

            const zcl_bridge_api_t *api = zcl_bridge_api_get();
            if (api && api->report_level) {
                api->report_level(short_addr, msg->src_endpoint, level);
            }
        }
        break;

    default:
        zcl_metrics_record_error(ZCL_ERR_SEVERITY_PERMANENT, TAG,
                                  "0x%04x: unhandled attr 0x%04x", short_addr, msg->attribute.id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

const zcl_device_handler_t level_control_handler = {
    .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
    .name = "level_control",
    .on_report_attr = level_report_attr,
};
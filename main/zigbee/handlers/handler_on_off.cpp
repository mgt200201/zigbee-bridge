/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_on_off.cpp
 *
 * @brief On/Off cluster (0x0006) handler 实现
 *
 * 新增属性的标准步骤：
 *   1. on_off_report_attr() switch 里加 case
 */

#include "handler_on_off.h"
#include "zcl_bridge_api.h"
#include "zcl_metrics.h"
#include <esp_log.h>
#include <ha/esp_zigbee_ha_standard.h>

static const char *TAG = "on_off";

static esp_err_t on_off_report_attr(const esp_zb_zcl_report_attr_message_t *msg)
{
    uint16_t short_addr = zcl_get_short_addr(msg->src_endpoint, &msg->src_address);
    if (short_addr == 0xFFFF) {
        return ESP_ERR_NOT_FOUND;
    }

    switch (msg->attribute.id) {

    case ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID:
        if (msg->attribute.data.size >= 1) {
            bool is_on = *(uint8_t *)msg->attribute.data.value != 0;
            ESP_LOGI(TAG, "0x%04x ep=%d: %s", short_addr, msg->src_endpoint, is_on ? "ON" : "OFF");

            const zcl_bridge_api_t *api = zcl_bridge_api_get();
            if (api && api->report_on_off) {
                api->report_on_off(short_addr, msg->src_endpoint, is_on);
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

const zcl_device_handler_t on_off_handler = {
    .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
    .name = "on_off",
    .on_report_attr = on_off_report_attr,
};
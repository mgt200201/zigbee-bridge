/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_occupancy.cpp
 *
 * @brief Occupancy Sensing cluster (0x0406) handler 实现
 *
 * Occupancy 为 bitmap8，bit0 表示有人。传感器类型属性随 Configure Reporting
 * 偶发上报，仅打印，不转发到 Matter。
 */

#include "handler_occupancy.h"
#include "zcl_bridge_api.h"
#include "zcl_metrics.h"
#include <esp_log.h>
#include <zcl/esp_zigbee_zcl_occupancy_sensing.h>

static const char *TAG = "occupancy";

/* ==================== 属性报告处理 ==================== */

static esp_err_t occupancy_report_attr(const esp_zb_zcl_report_attr_message_t *msg)
{
    uint16_t short_addr = zcl_get_short_addr(msg->src_endpoint, &msg->src_address);
    if (short_addr == 0xFFFF) {
        return ESP_ERR_NOT_FOUND;
    }

    switch (msg->attribute.id) {

    case ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_ID:
        if (msg->attribute.data.size >= 1) {
            uint8_t val = *(uint8_t *)msg->attribute.data.value;
            ESP_LOGI(TAG, "0x%04x ep=%d: occupancy=0x%02x", short_addr, msg->src_endpoint, val);

            const zcl_bridge_api_t *api = zcl_bridge_api_get();
            if (api && api->report_sensor) {
                api->report_sensor(short_addr, msg->src_endpoint, 0x0406, (int16_t)val);
            }
        }
        break;

    case ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_ID:
    case ESP_ZB_ZCL_ATTR_OCCUPANCY_SENSING_OCCUPANCY_SENSOR_TYPE_BITMAP_ID:
        if (msg->attribute.data.size >= 1) {
            uint8_t val = *(uint8_t *)msg->attribute.data.value;
            ESP_LOGD(TAG, "0x%04x attr 0x%04x: 0x%02x", short_addr, msg->attribute.id, val);
        }
        break;

    default:
        zcl_metrics_record_error(ZCL_ERR_SEVERITY_PERMANENT, TAG,
                                  "0x%04x: unhandled attr 0x%04x", short_addr, msg->attribute.id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

const zcl_device_handler_t occupancy_handler = {
    .cluster_id = 0x0406,
    .name = "occupancy",
    .on_report_attr = occupancy_report_attr,
};

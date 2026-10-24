/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_illuminance.cpp
 *
 * @brief Illuminance Measurement cluster (0x0400) handler 实现
 *
 * MeasuredValue 为对数刻度（mv = 10000*log10(lux)+1），
 * 与 Matter IlluminanceMeasurement 的量纲一致，原样透传。
 */

#include "handler_illuminance.h"
#include "zcl_bridge_api.h"
#include "zcl_metrics.h"
#include <esp_log.h>
#include <zcl/esp_zigbee_zcl_illuminance_meas.h>

static const char *TAG = "illum";

/* ==================== 属性报告处理 ==================== */

static esp_err_t illum_report_attr(const esp_zb_zcl_report_attr_message_t *msg)
{
    uint16_t short_addr = zcl_get_short_addr(msg->src_endpoint, &msg->src_address);
    if (short_addr == 0xFFFF) {
        return ESP_ERR_NOT_FOUND;
    }

    switch (msg->attribute.id) {

    case ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID:
        if (msg->attribute.data.size >= 2) {
            uint16_t val = *(uint16_t *)msg->attribute.data.value;
            ESP_LOGI(TAG, "0x%04x ep=%d: measured=0x%04x", short_addr, msg->src_endpoint, val);

            const zcl_bridge_api_t *api = zcl_bridge_api_get();
            if (api && api->report_sensor) {
                /* report_sensor 用 int16 承载，0xFFFE 等值按位往返无损 */
                api->report_sensor(short_addr, msg->src_endpoint, 0x0400, (int16_t)val);
            }
        }
        break;

    case ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MIN_MEASURED_VALUE_ID:
    case ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MAX_MEASURED_VALUE_ID:
        if (msg->attribute.data.size >= 2) {
            uint16_t val = *(uint16_t *)msg->attribute.data.value;
            ESP_LOGD(TAG, "0x%04x attr 0x%04x: 0x%04x", short_addr, msg->attribute.id, val);
        }
        break;

    default:
        zcl_metrics_record_error(ZCL_ERR_SEVERITY_PERMANENT, TAG,
                                  "0x%04x: unhandled attr 0x%04x", short_addr, msg->attribute.id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

const zcl_device_handler_t illuminance_handler = {
    .cluster_id = 0x0400,
    .name = "illuminance",
    .on_report_attr = illum_report_attr,
};

/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_humidity.cpp
 *
 * @brief Relative Humidity Measurement cluster (0x0405) handler 实现
 *
 * 新增属性的标准步骤：
 *   1. humid_report_attr() switch 里加 case
 *   2. humid_read_resp() 同样加 case
 */

#include "handler_humidity.h"
#include "zcl_bridge_api.h"
#include "zcl_metrics.h"
#include <esp_log.h>
#include <zcl/esp_zigbee_zcl_humidity_meas.h>

static const char *TAG = "humid";

/* ==================== 属性报告处理 ==================== */

static esp_err_t humid_report_attr(const esp_zb_zcl_report_attr_message_t *msg)
{
    uint16_t short_addr = zcl_get_short_addr(msg->src_endpoint, &msg->src_address);
    if (short_addr == 0xFFFF) {
        return ESP_ERR_NOT_FOUND;
    }

    switch (msg->attribute.id) {

    case ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID:
        if (msg->attribute.data.size >= 2) {
            uint16_t val = *(uint16_t *)msg->attribute.data.value;
            ESP_LOGI(TAG, "0x%04x ep=%d: %.2f%%", short_addr, msg->src_endpoint, val * 0.01f);

            const zcl_bridge_api_t *api = zcl_bridge_api_get();
            if (api && api->report_sensor) {
                api->report_sensor(short_addr, msg->src_endpoint, 0x0405, (int16_t)val);
            }
        }
        break;

    case ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_MIN_VALUE_ID:
    case ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_MAX_VALUE_ID:
        if (msg->attribute.data.size >= 2) {
            uint16_t val = *(uint16_t *)msg->attribute.data.value;
            ESP_LOGD(TAG, "0x%04x attr 0x%04x: %.2f%%", short_addr, msg->attribute.id, val * 0.01f);
        }
        break;

    default:
        zcl_metrics_record_error(ZCL_ERR_SEVERITY_PERMANENT, TAG,
                                  "0x%04x: unhandled attr 0x%04x", short_addr, msg->attribute.id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

/* ==================== 读属性响应处理 ==================== */

static esp_err_t humid_read_resp(const esp_zb_zcl_cmd_read_attr_resp_message_t *msg)
{
    uint16_t short_addr = msg->info.src_address.u.short_addr;
    if (short_addr == 0xFFFF) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_zb_zcl_read_attr_resp_variable_t *var = msg->variables;
    if (!var) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (var->attribute.id) {

    case ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID:
        if (var->attribute.data.size >= 2) {
            uint16_t val = *(uint16_t *)var->attribute.data.value;
            ESP_LOGI(TAG, "Read 0x%04x: %.2f%%", short_addr, val * 0.01f);

            const zcl_bridge_api_t *api = zcl_bridge_api_get();
            if (api && api->report_sensor) {
                api->report_sensor(short_addr, msg->info.src_endpoint, 0x0405, (int16_t)val);
            }
        }
        break;

    default:
        ESP_LOGW(TAG, "Read resp: unhandled attr 0x%04x from 0x%04x",
                 var->attribute.id, short_addr);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

const zcl_device_handler_t humidity_handler = {
    .cluster_id = 0x0405,
    .name = "humidity",
    .on_report_attr = humid_report_attr,
    .on_read_attr_resp = humid_read_resp,
};
/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_power_config.cpp
 *
 * @brief Power Configuration cluster (0x0001) handler 实现
 *
 * TH-01 等电池供电设备通过该集群上报电量，转成 Matter PowerSource 集群：
 *   - BatteryPercentageRemaining (0x0021) 0~200，单位 0.5%
 *   - BatteryVoltage (0x0020) 单位 100mV
 * 0xFF 是 ZCL 约定的"未知"，不能当真实值写入 Matter。
 */

#include "handler_power_config.h"
#include "zcl_bridge_api.h"
#include "zcl_metrics.h"
#include <esp_log.h>
#include <zcl/esp_zigbee_zcl_power_config.h>

static const char *TAG = "power_cfg";

#define ZCL_BATTERY_UNKNOWN 0xFF

/* ==================== 属性报告处理 ==================== */

static esp_err_t power_config_report_attr(const esp_zb_zcl_report_attr_message_t *msg)
{
    uint16_t short_addr = zcl_get_short_addr(msg->src_endpoint, &msg->src_address);
    if (short_addr == 0xFFFF) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 只转发本次上报的属性，另一项用 0xFF 表示"本次没有" */
    uint8_t percent = ZCL_BATTERY_UNKNOWN;
    uint8_t voltage = ZCL_BATTERY_UNKNOWN;

    switch (msg->attribute.id) {

    case ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID:
        if (msg->attribute.data.size >= 1) {
            percent = *(uint8_t *)msg->attribute.data.value;
            if (percent != ZCL_BATTERY_UNKNOWN) {
                ESP_LOGI(TAG, "0x%04x ep=%d: battery %u.%u%%", short_addr, msg->src_endpoint,
                         percent / 2, (percent % 2) * 5);
            }
        }
        break;

    case ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID:
        if (msg->attribute.data.size >= 1) {
            voltage = *(uint8_t *)msg->attribute.data.value;
            if (voltage != ZCL_BATTERY_UNKNOWN) {
                ESP_LOGI(TAG, "0x%04x ep=%d: battery voltage %u.%uV", short_addr, msg->src_endpoint,
                         voltage / 10, voltage % 10);
            }
        }
        break;

    default:
        zcl_metrics_record_error(ZCL_ERR_SEVERITY_PERMANENT, TAG,
                                  "0x%04x: unhandled attr 0x%04x", short_addr, msg->attribute.id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (percent == ZCL_BATTERY_UNKNOWN && voltage == ZCL_BATTERY_UNKNOWN) {
        return ESP_OK;
    }

    const zcl_bridge_api_t *api = zcl_bridge_api_get();
    if (api && api->report_battery) {
        api->report_battery(short_addr, msg->src_endpoint, percent, voltage);
    }

    return ESP_OK;
}

const zcl_device_handler_t power_config_handler = {
    .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
    .name = "power_config",
    .on_report_attr = power_config_report_attr,
    .on_set_attr = NULL,
    .on_read_attr_resp = NULL,
};

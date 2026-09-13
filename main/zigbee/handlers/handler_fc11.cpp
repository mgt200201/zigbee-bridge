/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_fc11.cpp
 *
 * @brief 私有集群 0xFC11 处理器实现
 *
 * 新增属性的标准步骤：
 *   1. handler_fc11.h 加 #define FC11_ATTR_XXX
 *   2. 本文件 fc11_handle_report() 加 case
 *   3. 本文件 fc11_handle_read_resp() 加 case
 */

#include "handler_fc11.h"
#include "zcl_bridge_api.h"
#include "zcl_metrics.h"
#include <esp_log.h>

static const char *TAG = "fc11";

/* ==================== 属性报告处理 ==================== */

static esp_err_t fc11_handle_report(const esp_zb_zcl_report_attr_message_t *msg)
{
    uint16_t short_addr = zcl_get_short_addr(msg->src_endpoint, &msg->src_address);
    if (short_addr == 0xFFFF) {
        return ESP_ERR_NOT_FOUND;
    }

    switch (msg->attribute.id) {

    case FC11_ATTR_REGISTER_STATUS:
        if (msg->attribute.data.size >= 2) {
            uint16_t val = *(uint16_t *)msg->attribute.data.value;
            ESP_LOGI(TAG, "0x%04x ep=%d register_status=0x%04x", short_addr, msg->src_endpoint, val);

            const zcl_bridge_api_t *api = zcl_bridge_api_get();
            if (api && api->report_sensor) {
                api->report_sensor(short_addr, msg->src_endpoint, 0xFC11, (int16_t)val);
            }
        }
        break;

    default:
        zcl_metrics_record_error(ZCL_ERR_SEVERITY_PERMANENT, TAG,
                                  "0x%04x: unhandled attr 0x%04x in report",
                                  short_addr, msg->attribute.id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

/* ==================== 读属性响应处理 ==================== */

static esp_err_t fc11_handle_read_resp(const esp_zb_zcl_cmd_read_attr_resp_message_t *msg)
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

    case FC11_ATTR_REGISTER_STATUS:
        if (var->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 && var->attribute.data.size >= 2) {
            uint16_t val = *(uint16_t *)var->attribute.data.value;
            ESP_LOGI(TAG, "Read 0x%04x register_status=0x%04x", short_addr, val);

            const zcl_bridge_api_t *api = zcl_bridge_api_get();
            if (api && api->report_sensor) {
                api->report_sensor(short_addr, msg->info.src_endpoint, 0xFC11, (int16_t)val);
            }
        }
        break;

    default:
        ESP_LOGW(TAG, "Unhandled read resp attr 0x%04x from 0x%04x",
                 var->attribute.id, short_addr);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

/* ==================== ZCL 适配器 ==================== */

static esp_err_t fc11_on_report_attr(const esp_zb_zcl_report_attr_message_t *msg)
{
    return fc11_handle_report(msg);
}

static esp_err_t fc11_on_read_attr_resp(const esp_zb_zcl_cmd_read_attr_resp_message_t *msg)
{
    return fc11_handle_read_resp(msg);
}

const zcl_device_handler_t fc11_handler = {
    .cluster_id = 0xFC11,
    .name = "fc11",
    .on_report_attr = fc11_on_report_attr,
    .on_set_attr = NULL,
    .on_read_attr_resp = fc11_on_read_attr_resp,
};
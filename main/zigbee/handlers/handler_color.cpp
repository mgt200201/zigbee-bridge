/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_color.cpp
 *
 * @brief Color Control cluster (0x0300) handler 实现
 *
 * Zigbee 灯一次只上报一个颜色属性（CurrentX 或 CurrentY，或
 * CurrentHue / CurrentSaturation）。而命令/上报往往需要成对的值，
 * 因此这里维护一张按短地址索引的"影子表"，凑齐一对才向上转发：
 *   - vailid group XY: CurrentX + CurrentY → report_color_xy
 *   - valid group HS: CurrentHue + CurrentSaturation → report_color_hsv
 */

#include "handler_color.h"
#include "zcl_bridge_api.h"
#include "zcl_metrics.h"
#include <esp_log.h>
#include <ha/esp_zigbee_ha_standard.h>

static const char *TAG = "color";

/* ==================== 影子表（按短地址索引） ==================== */

#define COLOR_SHADOW_SLOTS 8

typedef struct {
    uint16_t short_addr;    /**< 0/0xFFFF 表示空槽 */
    uint16_t x, y;          /**< CurrentX / CurrentY */
    uint8_t hue, sat;       /**< CurrentHue / CurrentSaturation */
    bool have_xy;           /**< X 和 Y 都收到过 */
    bool have_hs;           /**< H 和 S 都收到过 */
} color_shadow_t;

static color_shadow_t s_shadow[COLOR_SHADOW_SLOTS];

static color_shadow_t *shadow_find(uint16_t short_addr, bool create)
{
    for (int i = 0; i < COLOR_SHADOW_SLOTS; i++) {
        if (s_shadow[i].short_addr == short_addr) {
            return &s_shadow[i];
        }
    }
    if (create) {
        for (int i = 0; i < COLOR_SHADOW_SLOTS; i++) {
            if (s_shadow[i].short_addr == 0 || s_shadow[i].short_addr == 0xFFFF) {
                s_shadow[i].short_addr = short_addr;
                s_shadow[i].have_xy = false;
                s_shadow[i].have_hs = false;
                return &s_shadow[i];
            }
        }
    }
    return NULL;
}

static esp_err_t color_report_attr(const esp_zb_zcl_report_attr_message_t *msg)
{
    uint16_t short_addr = zcl_get_short_addr(msg->src_endpoint, &msg->src_address);
    if (short_addr == 0xFFFF) {
        return ESP_ERR_NOT_FOUND;
    }

    const zcl_bridge_api_t *api = zcl_bridge_api_get();

#define COLOR_VAL_U16() (msg->attribute.data.size >= 2 ? *(uint16_t *)msg->attribute.data.value : 0)
#define COLOR_VAL_U8()  (msg->attribute.data.size >= 1 ? *(uint8_t *)msg->attribute.data.value : 0)

    switch (msg->attribute.id) {

    case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID:
        {
            color_shadow_t *s = shadow_find(short_addr, true);
            if (!s) return ESP_ERR_NO_MEM;
            s->x = COLOR_VAL_U16();
            ESP_LOGI(TAG, "0x%04x ep=%d: x=%u", short_addr, msg->src_endpoint, s->x);
            if (s->have_xy && api && api->report_color_xy) {
                api->report_color_xy(short_addr, msg->src_endpoint, s->x, s->y);
            }
        }
        break;

    case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID:
        {
            color_shadow_t *s = shadow_find(short_addr, true);
            if (!s) return ESP_ERR_NO_MEM;
            s->y = COLOR_VAL_U16();
            s->have_xy = true;
            ESP_LOGI(TAG, "0x%04x ep=%d: y=%u", short_addr, msg->src_endpoint, s->y);
            if (s->have_xy && api && api->report_color_xy) {
                api->report_color_xy(short_addr, msg->src_endpoint, s->x, s->y);
            }
        }
        break;

    case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID:
        {
            color_shadow_t *s = shadow_find(short_addr, true);
            if (!s) return ESP_ERR_NO_MEM;
            s->hue = COLOR_VAL_U8();
            ESP_LOGI(TAG, "0x%04x ep=%d: hue=%u", short_addr, msg->src_endpoint, s->hue);
            if (s->have_hs && api && api->report_color_hsv) {
                api->report_color_hsv(short_addr, msg->src_endpoint, s->hue, s->sat);
            }
        }
        break;

    case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID:
        {
            color_shadow_t *s = shadow_find(short_addr, true);
            if (!s) return ESP_ERR_NO_MEM;
            s->sat = COLOR_VAL_U8();
            s->have_hs = true;
            ESP_LOGI(TAG, "0x%04x ep=%d: sat=%u", short_addr, msg->src_endpoint, s->sat);
            if (s->have_hs && api && api->report_color_hsv) {
                api->report_color_hsv(short_addr, msg->src_endpoint, s->hue, s->sat);
            }
        }
        break;

    case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID:
        {
            uint16_t mireds = COLOR_VAL_U16();
            ESP_LOGI(TAG, "0x%04x ep=%d: mireds=%u", short_addr, msg->src_endpoint, mireds);
            if (api && api->report_color_temperature) {
                api->report_color_temperature(short_addr, msg->src_endpoint, mireds);
            }
        }
        break;

    case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID:
    case ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_COLOR_MODE_ID:
        {
            uint8_t mode = COLOR_VAL_U8();
            ESP_LOGI(TAG, "0x%04x ep=%d: color_mode=0x%02x", short_addr, msg->src_endpoint, mode);
            if (api && api->report_color_mode) {
                api->report_color_mode(short_addr, msg->src_endpoint, mode);
            }
        }
        break;

    default:
        zcl_metrics_record_error(ZCL_ERR_SEVERITY_PERMANENT, TAG,
                                  "0x%04x: unhandled attr 0x%04x", short_addr, msg->attribute.id);
        return ESP_ERR_NOT_SUPPORTED;
    }

#undef COLOR_VAL_U16
#undef COLOR_VAL_U8

    return ESP_OK;
}

/* ==================== 读属性响应 ====================
 * ColorMode 无法通过 Report Attributes 上报（ZBOSS 会对该属性断言），
 * 因此由网关主动读取校准：把结果写入 Matter 端，覆盖此前的推断值。 */
static esp_err_t color_read_attr_resp(const esp_zb_zcl_cmd_read_attr_resp_message_t *msg)
{
    if (!msg) {
        return ESP_FAIL;
    }
    uint16_t short_addr = msg->info.src_address.u.short_addr;
    if (short_addr == 0xFFFF) {
        return ESP_ERR_NOT_FOUND;
    }

    const zcl_bridge_api_t *api = zcl_bridge_api_get();

    for (esp_zb_zcl_read_attr_resp_variable_t *var = msg->variables; var; var = var->next) {
        bool ok = (var->status == ESP_ZB_ZCL_STATUS_SUCCESS)
                  && (var->attribute.data.value != NULL)
                  && (var->attribute.data.size >= 1);
        if (!ok) {
            ESP_LOGW(TAG, "0x%04x: read attr 0x%04x status=0x%02x", short_addr,
                     var->attribute.id, var->status);
            continue;
        }
        if (var->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID
            || var->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_ENHANCED_COLOR_MODE_ID) {
            uint8_t mode = *(uint8_t *)var->attribute.data.value;
            ESP_LOGI(TAG, "0x%04x ep=%d: attr 0x%04x color_mode=0x%02x",
                     short_addr, msg->info.src_endpoint, var->attribute.id, mode);
            if (api && api->report_color_mode) {
                api->report_color_mode(short_addr, msg->info.src_endpoint, mode);
            }
        }
    }

    return ESP_OK;
}

const zcl_device_handler_t color_control_handler = {
    .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
    .name = "color_control",
    .on_report_attr = color_report_attr,
    .on_read_attr_resp = color_read_attr_resp,
};
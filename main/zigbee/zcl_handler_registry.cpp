/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @brief ZCL 消息路由核心实现
 *
 * 所有集群按 cluster_id 注册 handler。收到属性报告时按 cluster_id 匹配。
 * 每个 handler 内部通过 switch(attr_id) 处理不同的属性。
 *
 * 新增 cluster = 写 handler → zcl_handlers_init() 注册一行。
 *
 * Handler 实现位于 handlers/ 目录，每个 cluster 一个独立文件：
 *   handler_temperature.cpp  (0x0402)
 *   handler_humidity.cpp     (0x0405)
 *   handler_on_off.cpp       (0x0006)
 *   handler_fc11.cpp         (0xFC11)
 */

#include "zcl_handler_registry.h"
#include "zcl_bridge_api.h"
#include "zcl_metrics.h"
#include "handlers/handler_temperature.h"
#include "handlers/handler_humidity.h"
#include "handlers/handler_on_off.h"
#include "handlers/handler_fc11.h"
#include <esp_log.h>

static const char *TAG = "zcl_registry";

/* ==================================================================
 *  Handler 注册表
 *
 *  Kconfig 控制大小，线性遍历查找。
 *  handler 数量 < 50 时线性遍历效率足够。
 * ================================================================== */

#ifndef CONFIG_ZIGBEE_MAX_HANDLERS
#define CONFIG_ZIGBEE_MAX_HANDLERS 32
#endif

static const zcl_device_handler_t *s_handlers[CONFIG_ZIGBEE_MAX_HANDLERS];
static int s_handler_count = 0;

static const zcl_device_handler_t *find_handler(uint16_t cluster_id)
{
    for (int i = 0; i < s_handler_count; i++) {
        if (s_handlers[i]->cluster_id == cluster_id) {
            return s_handlers[i];
        }
    }
    return NULL;
}

/* ==================== 注册 API ==================== */

esp_err_t zcl_handler_register(const zcl_device_handler_t *handler)
{
    if (!handler || !handler->name) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_handler_count >= CONFIG_ZIGBEE_MAX_HANDLERS) {
        ESP_LOGE(TAG, "Registry full (max %d, increase CONFIG_ZIGBEE_MAX_HANDLERS)",
                 CONFIG_ZIGBEE_MAX_HANDLERS);
        return ESP_ERR_NO_MEM;
    }

    /* 同 cluster 覆盖（支持运行时替换） */
    for (int i = 0; i < s_handler_count; i++) {
        if (s_handlers[i]->cluster_id == handler->cluster_id) {
            ESP_LOGW(TAG, "[UPDATE] cluster 0x%04x (%s)", handler->cluster_id, handler->name);
            s_handlers[i] = handler;
            return ESP_OK;
        }
    }

    s_handlers[s_handler_count++] = handler;
    ESP_LOGI(TAG, "[REG] cluster=0x%04x (%s) [%d/%d]",
             handler->cluster_id, handler->name,
             s_handler_count, CONFIG_ZIGBEE_MAX_HANDLERS);
    return ESP_OK;
}

/* ==================== 全局分发器 ==================== */

esp_err_t zcl_handler_dispatch(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (!message) {
        return ESP_FAIL;
    }

    switch (callback_id) {

    /* ──── 属性报告 ──── */
    case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
        const esp_zb_zcl_report_attr_message_t *msg =
            (const esp_zb_zcl_report_attr_message_t *)message;

        if (msg->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
            return ESP_ERR_INVALID_ARG;
        }

        /* 查 handler 表：按 cluster_id 匹配 */
        const zcl_device_handler_t *h = find_handler(msg->cluster);
        if (h && h->on_report_attr) {
            esp_err_t err = h->on_report_attr(msg);
            zcl_metrics_record_dispatch(true);
            return err;
        }

        /* 未处理 → 记录 metrics（PERMANENT：未知 cluster，需要运维介入） */
        zcl_metrics_record_dispatch(false);
        zcl_metrics_record_unhandled(msg->cluster);
        zcl_metrics_record_error(ZCL_ERR_SEVERITY_PERMANENT, TAG,
                                  "Unhandled report: cluster=0x%04x attr=0x%04x from 0x%04x ep=%d",
                                  msg->cluster, msg->attribute.id,
                                  zcl_get_short_addr(msg->src_endpoint, &msg->src_address),
                                  msg->src_endpoint);
        return ESP_OK;
    }

    /* ──── 属性写入：查自定义 handler 表 ──── */
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: {
        const esp_zb_zcl_set_attr_value_message_t *msg =
            (const esp_zb_zcl_set_attr_value_message_t *)message;

        const zcl_device_handler_t *h = find_handler(msg->info.cluster);
        if (h && h->on_set_attr) {
            esp_err_t err = h->on_set_attr(msg);
            zcl_metrics_record_dispatch(true);
            return err;
        }
        /* 没有 set_attr handler 是正常的（传感器只上报不接收） */
        return ESP_OK;
    }

    /* ──── 读取响应：查自定义 handler 表 ──── */
    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID: {
        const esp_zb_zcl_cmd_read_attr_resp_message_t *msg =
            (const esp_zb_zcl_cmd_read_attr_resp_message_t *)message;

        const zcl_device_handler_t *h = find_handler(msg->info.cluster);
        if (h && h->on_read_attr_resp) {
            esp_err_t err = h->on_read_attr_resp(msg);
            zcl_metrics_record_dispatch(true);
            return err;
        }
        /* 返回 NOT_FOUND 让 zdp_discovery 走 Basic Cluster fallback */
        return ESP_ERR_NOT_FOUND;
    }

    default:
        return ESP_OK;
    }
}

/* ==================== 工具函数 ==================== */

int zcl_handler_count(void)
{
    return s_handler_count;
}

uint16_t zcl_get_short_addr(uint8_t src_endpoint, const esp_zb_zcl_addr_t *src_address)
{
    /* 优先用源短地址（最常见路径） */
    if (src_address && src_address->addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT &&
        src_address->u.short_addr != 0xFFFF) {
        return src_address->u.short_addr;
    }
    return 0xFFFF;
}

/* ==================== 初始化 ==================== */

void zcl_handlers_init(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   ZCL Handler Registry Init          ║");
    ESP_LOGI(TAG, "╠══════════════════════════════════════╣");
    ESP_LOGI(TAG, "║  Max handlers: %-21d ║", CONFIG_ZIGBEE_MAX_HANDLERS);
    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");

    zcl_handler_register(&temp_handler);
    zcl_handler_register(&humidity_handler);
    zcl_handler_register(&on_off_handler);
    zcl_handler_register(&fc11_handler);

    /* ── 以后新增 cluster 只需在这里加一行 ── */
    /* zcl_handler_register(&illuminance_handler);    // 0x0400 */
    /* zcl_handler_register(&pressure_handler);       // 0x0401 */
    /* zcl_handler_register(&level_control_handler);  // 0x0008 */
    /* zcl_handler_register(&color_control_handler);  // 0x0300 */

    ESP_LOGI(TAG, "ZCL handlers ready: %d registered", s_handler_count);

    ESP_LOGI(TAG, "  [0x0402] temperature");
    ESP_LOGI(TAG, "  [0x0405] humidity");
    ESP_LOGI(TAG, "  [%s] on_off", s_handlers[2] ? s_handlers[2]->name : "?");
    ESP_LOGI(TAG, "  [0xFC11] fc11");
}
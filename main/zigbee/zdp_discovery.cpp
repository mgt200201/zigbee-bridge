/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zdp_discovery.cpp
 *
 * @brief Zigbee 设备发现与 ZCL 处理层
 *
 * 负责 ZDP 设备发现（Match Descriptor / Simple Descriptor）、
 * ZCL 属性报告接收、Read Attributes 响应解析。
 *
 * 当发现新设备或收到数据时，调用 matter/bridge_manager 接口
 * 完成桥接设备创建或属性更新。
 *
 * 本层是 Zigbee 协议栈的最上层，直接调用 Matter 层接口。
 */

#include "zdp_discovery.h"
#include <zboss_manager.h>
#include <bridge_manager.h>
#include <string.h>
#include <esp_log.h>
#include <zdo/esp_zigbee_zdo_command.h>
#include <ha/esp_zigbee_ha_standard.h>
#include <device_info.h>

static const char *TAG = "zdp_discovery";

/** 待处理的设备公告数据 */
static uint16_t s_pending_annce_short_addr = 0xFFFF;
static esp_zb_ieee_addr_t s_pending_annce_ieee = {0};

/* ==================== 内部函数声明 ==================== */
static void find_temp_humidity_sensor_cb(uint8_t param);
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message);
static esp_err_t zb_set_attr_value_handler(const esp_zb_zcl_set_attr_value_message_t *message);
static esp_err_t zb_report_attr_handler(const esp_zb_zcl_report_attr_message_t *message);
static esp_err_t zb_read_attr_resp_handler(const esp_zb_zcl_cmd_read_attr_resp_message_t *message);
static void parse_and_log_attribute(uint8_t src_ep, uint8_t dst_ep, uint16_t cluster, const esp_zb_zcl_attribute_t *attribute);

/* ==================== Simple Descriptor 响应回调 ==================== */

/**
 * @brief Simple Descriptor 响应回调
 *
 * 在 Match Descriptor 成功后，发送 Simple Descriptor Request，
 * 此回调接收并解析子设备的 Simple Descriptor 信息，
 * 然后创建 Matter 桥接设备并读取 Basic 属性。
 */
static void simple_desc_cb(esp_zb_zdp_status_t zdo_status,
                           esp_zb_af_simple_desc_1_1_t *simple_desc,
                           void *user_ctx)
{
    uint16_t addr = (uint16_t)(uintptr_t)user_ctx;

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !simple_desc) {
        ESP_LOGW(TAG, "Simple Descriptor request failed for 0x%04x (status=%d)", addr, zdo_status);
        return;
    }

    uint8_t endpoint = simple_desc->endpoint;

    /* 打印 Simple Descriptor 信息 */
    ESP_LOGI(TAG, "=== Simple Descriptor for device 0x%04x, endpoint=%d ===", addr, endpoint);
    ESP_LOGI(TAG, "  Profile ID:    0x%04x", simple_desc->app_profile_id);
    ESP_LOGI(TAG, "  Device ID:     0x%04x", simple_desc->app_device_id);
    ESP_LOGI(TAG, "  Input Clusters:  %d", simple_desc->app_input_cluster_count);
    for (int i = 0; i < simple_desc->app_input_cluster_count; i++) {
        ESP_LOGI(TAG, "    [%d] 0x%04x", i, simple_desc->app_cluster_list[i]);
    }

    /* 调用 Matter 层创建桥接设备并读取 Basic 属性 */
    bridge_manager_on_device_discovered(addr, endpoint);
}

/* ==================== Match Descriptor 响应回调 ==================== */

/**
 * @brief Match Descriptor 响应回调
 *
 * 由 ZDO Match Descriptor 请求触发，成功时发送 Simple Descriptor Request。
 */
static void match_desc_resp_cb(esp_zb_zdp_status_t zdo_status,
                               uint16_t addr, uint8_t endpoint,
                               void *user_ctx)
{
    ESP_LOGI(TAG, "Match Desc response: addr=0x%04x, ep=%d, status=%d",
             addr, endpoint, zdo_status);

    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        /* 发送 Simple Descriptor Request 获取完整的端点信息 */
        esp_zb_zdo_simple_desc_req_param_t req;
        req.addr_of_interest = addr;
        req.endpoint = endpoint;
        esp_zb_zdo_simple_desc_req(&req, simple_desc_cb, (void *)(uintptr_t)addr);
    }
}

/* ==================== 设备发现调度 ==================== */

/**
 * @brief 延迟执行的设备发现函数
 *
 * 由 scheduler 在收到 Device Annce 后 500ms 调用，
 * 向目标设备发送 ZDO Match Descriptor 请求。
 */
static void find_temp_humidity_sensor_cb(uint8_t param)
{
    (void)param;
    if (s_pending_annce_short_addr == 0xFFFF) {
        return;
    }
    uint16_t short_addr = s_pending_annce_short_addr;
    s_pending_annce_short_addr = 0xFFFF;

    /* 检查是否已存在桥接设备 */
    if (bridge_manager_is_device_bridged(short_addr)) {
        ESP_LOGI(TAG, "Device 0x%04hx already bridged, re-reading basic attributes", short_addr);
        uint8_t ep = bridge_manager_get_endpoint_by_short(short_addr);
        if (ep != 0xFF) {
            zdp_discovery_read_basic_attributes(short_addr, ep);
        }
        return;
    }

    ESP_LOGI(TAG, "Discovering temp/humidity sensor on 0x%04hx", short_addr);
    esp_zb_zdo_match_desc_req_param_t cmd_req;
    cmd_req.dst_nwk_addr = short_addr;
    cmd_req.addr_of_interest = short_addr;
    cmd_req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    uint16_t cluster_list[] = { ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT };
    cmd_req.num_in_clusters = 1;
    cmd_req.num_out_clusters = 0;
    cmd_req.cluster_list = cluster_list;
    esp_zb_zdo_match_cluster(&cmd_req, match_desc_resp_cb, NULL);
}

void zdp_discovery_handle_device_annce(uint16_t short_addr,
                                       const esp_zb_ieee_addr_t ieee_addr,
                                       bool steering_active)
{
    /* 不再需要检查 steering_active，bridge_manager 会处理去重 */
    (void)steering_active;

    /* 记录待处理设备，延迟 500ms 再发现 */
    s_pending_annce_short_addr = short_addr;
    memcpy(s_pending_annce_ieee, ieee_addr, sizeof(esp_zb_ieee_addr_t));
    esp_zb_scheduler_alarm((esp_zb_callback_t)find_temp_humidity_sensor_cb, 0, 500);
}

/* ==================== ZCL 属性处理 ==================== */

static void parse_and_log_attribute(uint8_t src_ep, uint8_t dst_ep,
                                    uint16_t cluster,
                                    const esp_zb_zcl_attribute_t *attribute)
{
    ESP_LOGI(TAG, "ZCL report: src_ep=%d, dst_ep=%d, cluster=0x%04x, attr=0x%04x, size=%d",
             src_ep, dst_ep, cluster, attribute->id, attribute->data.size);

    switch (cluster) {
    case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
        if (attribute->id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && attribute->data.size == 1) {
            uint8_t on_off = *(uint8_t *)attribute->data.value;
            ESP_LOGI(TAG, "  -> On/Off state: %s", on_off ? "ON" : "OFF");
        }
        break;

    case ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT:
        if (attribute->id == ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID && attribute->data.size == 2) {
            int16_t raw = *(int16_t *)attribute->data.value;
            ESP_LOGI(TAG, "  -> Temperature: %.2f C (raw=%d)", raw / 100.0, raw);
        }
        break;

    case 0x0405:
        if (attribute->id == 0x0000 && attribute->data.size == 2) {
            uint16_t raw = *(uint16_t *)attribute->data.value;
            ESP_LOGI(TAG, "  -> Humidity: %.2f %% (raw=%u)", raw / 100.0, raw);
        }
        break;

    default:
        ESP_LOGI(TAG, "  -> Unhandled cluster 0x%04x", cluster);
        break;
    }
}

static esp_err_t zb_set_attr_value_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (!message) {
        return ESP_FAIL;
    }
    if (message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Received ZCL set attr with error status(%d)", message->info.status);
        return ESP_ERR_INVALID_ARG;
    }
    parse_and_log_attribute(message->info.dst_endpoint, message->info.dst_endpoint,
                            message->info.cluster, &message->attribute);
    return ESP_OK;
}

static esp_err_t zb_report_attr_handler(const esp_zb_zcl_report_attr_message_t *message)
{
    if (!message) {
        return ESP_FAIL;
    }
    if (message->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Received ZCL report with error status(%d)", message->status);
        return ESP_ERR_INVALID_ARG;
    }
    parse_and_log_attribute(message->src_endpoint, message->dst_endpoint,
                            message->cluster, &message->attribute);

    if (message->cluster != ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT &&
        message->cluster != 0x0405) {
        return ESP_OK;
    }

    uint16_t short_addr = zboss_manager_find_short_addr_by_ep(message->src_endpoint);
    if (short_addr == 0xFFFF) {
        /* 从 ZCL 报告的 src_address 获取短地址 */
        short_addr = message->src_address.u.short_addr;
    }

    if (short_addr == 0xFFFF) {
        ESP_LOGW(TAG, "Cannot determine short_addr for ZCL report on endpoint %d",
                 message->src_endpoint);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "ZCL report for bridging: src_ep=%d, short_addr=0x%04x, cluster=0x%04x",
             message->src_endpoint, short_addr, message->cluster);

    if (message->cluster == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT &&
        message->attribute.id == ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID &&
        message->attribute.data.size == 2) {
        int16_t raw = *(int16_t *)message->attribute.data.value;
        bridge_manager_report_sensor_data(short_addr, message->src_endpoint,
                                          ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, raw);
    } else if (message->cluster == 0x0405 &&
               message->attribute.id == 0x0000 &&
               message->attribute.data.size == 2) {
        int16_t raw = *(int16_t *)message->attribute.data.value;
        bridge_manager_report_sensor_data(short_addr, message->src_endpoint, 0x0405, raw);
    }
    return ESP_OK;
}

static esp_err_t zb_read_attr_resp_handler(const esp_zb_zcl_cmd_read_attr_resp_message_t *message)
{
    if (!message) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Read Attributes Response received");

    uint16_t short_addr = message->info.src_address.u.short_addr;
    uint8_t src_endpoint = message->info.src_endpoint;

    uint32_t device_id = DEVICE_ID_UNKNOWN;
    char uiid[MAX_UIID_LEN] = {0};
    bool got_device_id = false;
    char manufacturer[MAX_NAME_LEN] = {0};
    char model[MAX_NAME_LEN] = {0};
    char sw_version[16] = {0};

    esp_zb_zcl_read_attr_resp_variable_t *var = message->variables;
    while (var) {
        switch (var->attribute.id) {
        case ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID:
            if (var->status == ESP_ZB_ZCL_STATUS_SUCCESS && var->attribute.data.size > 0) {
                uint8_t *data = (uint8_t *)var->attribute.data.value;
                uint8_t len = data[0];
                if (len > 0) {
                    uint8_t copy_len = (len < MAX_NAME_LEN - 1) ? len : (MAX_NAME_LEN - 1);
                    memcpy(manufacturer, &data[1], copy_len);
                    manufacturer[copy_len] = '\0';
                }
                ESP_LOGI(TAG, "  -> Manufacturer Name: %.*s", len, &data[1]);
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID:
            if (var->status == ESP_ZB_ZCL_STATUS_SUCCESS && var->attribute.data.size > 0) {
                uint8_t *data = (uint8_t *)var->attribute.data.value;
                uint8_t len = data[0];
                if (len > 0) {
                    uint8_t copy_len = (len < MAX_NAME_LEN - 1) ? len : (MAX_NAME_LEN - 1);
                    memcpy(model, &data[1], copy_len);
                    model[copy_len] = '\0';
                }
                ESP_LOGI(TAG, "  -> Model Identifier: %.*s", len, &data[1]);
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID:
            if (var->status == ESP_ZB_ZCL_STATUS_SUCCESS && var->attribute.data.size > 0) {
                uint8_t *data = (uint8_t *)var->attribute.data.value;
                uint8_t len = data[0];
                if (len > 0) {
                    uint8_t copy_len = (len < (int)sizeof(sw_version) - 1) ? len : ((int)sizeof(sw_version) - 1);
                    memcpy(sw_version, &data[1], copy_len);
                    sw_version[copy_len] = '\0';
                }
                ESP_LOGI(TAG, "  -> SW Build ID: %.*s", len, &data[1]);
            }
            break;

        case 0x4001:
            if (var->status == ESP_ZB_ZCL_STATUS_SUCCESS && var->attribute.data.size >= 4) {
                device_id = *(uint32_t *)var->attribute.data.value;
                got_device_id = true;
                ESP_LOGI(TAG, "  -> deviceId (0x4001): 0x%08X", (unsigned int)device_id);
            }
            break;

        case 0x4002:
            if (var->status == ESP_ZB_ZCL_STATUS_SUCCESS && var->attribute.data.size > 0) {
                uint8_t *data = (uint8_t *)var->attribute.data.value;
                uint8_t len = data[0];
                if (len > 0) {
                    uint8_t copy_len = (len < MAX_UIID_LEN - 1) ? len : (MAX_UIID_LEN - 1);
                    memcpy(uiid, &data[1], copy_len);
                    uiid[copy_len] = '\0';
                }
                ESP_LOGI(TAG, "  -> uiid (0x4002): %.*s", len, &data[1]);
            }
            break;

        default:
            break;
        }
        var = var->next;
    }

    /* 调用 Matter 层处理设备标识 */
    if (got_device_id && device_id != DEVICE_ID_UNKNOWN && device_id != 0) {
        known_device_t *known = zboss_manager_find_by_short(short_addr);
        bridge_manager_on_device_identified(device_id, uiid, short_addr, src_endpoint,
                                            known ? known->ieee_addr : NULL);
    }

    return ESP_OK;
}

/* ==================== ZCL 动作处理器 ==================== */

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        return zb_set_attr_value_handler((const esp_zb_zcl_set_attr_value_message_t *)message);
    case ESP_ZB_CORE_REPORT_ATTR_CB_ID:
        return zb_report_attr_handler((const esp_zb_zcl_report_attr_message_t *)message);
    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID:
        return zb_read_attr_resp_handler((const esp_zb_zcl_cmd_read_attr_resp_message_t *)message);
    default:
        return ESP_OK;
    }
}

esp_zb_core_action_callback_t zdp_discovery_get_action_handler(void)
{
    return zb_action_handler;
}

/* ==================== 读取 Basic 属性 ==================== */

void zdp_discovery_read_basic_attributes(uint16_t short_addr, uint8_t endpoint)
{
    ESP_LOGI(TAG, "Reading Basic attributes from device 0x%04hx, endpoint=%d", short_addr, endpoint);

    static const uint16_t attr_list[] = {
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
        ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID,
        0x4001,
        0x4002,
    };

    esp_zb_zcl_read_attr_cmd_t cmd_req;
    memset(&cmd_req, 0, sizeof(cmd_req));
    cmd_req.zcl_basic_cmd.dst_addr_u.addr_short = short_addr;
    cmd_req.zcl_basic_cmd.dst_endpoint = endpoint;
    cmd_req.zcl_basic_cmd.src_endpoint = HA_GW_ENDPOINT;
    cmd_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd_req.clusterID = ESP_ZB_ZCL_CLUSTER_ID_BASIC;
    cmd_req.attr_number = sizeof(attr_list) / sizeof(attr_list[0]);
    cmd_req.attr_field = (uint16_t *)attr_list;
    cmd_req.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    uint8_t seq = esp_zb_zcl_read_attr_cmd_req(&cmd_req);
    ESP_LOGI(TAG, "Read Attributes command sent, seq=%u", seq);
}

void zdp_discovery_init(void)
{
    /* 预留：后续可注册额外的 ZDO 回调等 */
}
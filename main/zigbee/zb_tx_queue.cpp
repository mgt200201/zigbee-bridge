/**
 * @file zb_tx_queue.cpp
 *
 * All Zigbee TX goes through one task that holds esp_zb_lock and sends
 * a single command per slice. Matter/CHIP and ZCL callbacks only enqueue.
 * That avoids zb_bufpool asserts from:
 *   - calling ZBOSS from the CHIP event loop
 *   - allocating a new buffer while a ZCL/ZDO callback still holds one
 */

#include "zb_tx_queue.h"
#include "zboss_manager.h"
#include "zdp_discovery.h"
#include "device_info.h"

#include <string.h>
#include <esp_log.h>
#include <esp_zigbee_core.h>
#include <zdo/esp_zigbee_zdo_command.h>
#include <zcl/esp_zigbee_zcl_command.h>
#include <zcl/esp_zigbee_zcl_on_off.h>
#include <zcl/esp_zigbee_zcl_level.h>
#include <zcl/esp_zigbee_zcl_color_control.h>
#include <zcl/esp_zigbee_zcl_basic.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char *TAG = "zb_tx";

#define ZB_TX_Q_LEN          32
#define ZB_TX_GAP_MS         40
#define ZB_TX_LOCK_WAIT_MS   1000

typedef enum {
    ZB_TX_ON_OFF = 1,
    ZB_TX_MOVE_TO_LEVEL,
    ZB_TX_MOVE_TO_COLOR,
    ZB_TX_MOVE_TO_HUE_SAT,
    ZB_TX_MOVE_TO_COLOR_TEMP,
    ZB_TX_WRITE_U16,
    ZB_TX_CONFIG_REPORT,
    ZB_TX_BIND,
    ZB_TX_READ_BASIC,
    ZB_TX_SIMPLE_DESC,
} zb_tx_kind_t;

typedef struct {
    zb_tx_kind_t kind;
    uint16_t short_addr;
    uint8_t endpoint;
    uint16_t cluster;
    uint16_t attr;
    uint16_t u16_a;
    uint16_t u16_b;
    uint16_t max_interval;
    uint8_t u8_a;
    uint8_t u8_b;
    uint8_t attr_type;
    esp_zb_ieee_addr_t ieee;
} zb_tx_item_t;

static zb_tx_item_t s_q[ZB_TX_Q_LEN];
static uint8_t s_head;
static uint8_t s_count;
static SemaphoreHandle_t s_mtx;
static TaskHandle_t s_task;
static volatile bool s_paused;

static bool is_coalesce_kind(zb_tx_kind_t kind)
{
    return kind == ZB_TX_ON_OFF || kind == ZB_TX_MOVE_TO_LEVEL ||
           kind == ZB_TX_MOVE_TO_COLOR || kind == ZB_TX_MOVE_TO_HUE_SAT ||
           kind == ZB_TX_MOVE_TO_COLOR_TEMP;
}

static bool same_target(const zb_tx_item_t *a, const zb_tx_item_t *b)
{
    return a->kind == b->kind && a->short_addr == b->short_addr && a->endpoint == b->endpoint;
}

static void enqueue(const zb_tx_item_t *item)
{
    if (!s_mtx || !s_task) {
        ESP_LOGW(TAG, "TX queue not ready, drop kind=%d", (int)item->kind);
        return;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (is_coalesce_kind(item->kind)) {
        for (uint8_t i = 0; i < s_count; i++) {
            uint8_t idx = (uint8_t)((s_head + i) % ZB_TX_Q_LEN);
            if (same_target(&s_q[idx], item)) {
                s_q[idx] = *item;
                xSemaphoreGive(s_mtx);
                xTaskNotifyGive(s_task);
                return;
            }
        }
    }
    if (s_count >= ZB_TX_Q_LEN) {
        ESP_LOGW(TAG, "TX queue full, drop kind=%d dst=0x%04x", (int)item->kind, item->short_addr);
        xSemaphoreGive(s_mtx);
        return;
    }
    uint8_t tail = (uint8_t)((s_head + s_count) % ZB_TX_Q_LEN);
    s_q[tail] = *item;
    s_count++;
    xSemaphoreGive(s_mtx);
    xTaskNotifyGive(s_task);
}

static bool dequeue(zb_tx_item_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_count == 0) {
        xSemaphoreGive(s_mtx);
        return false;
    }
    *out = s_q[s_head];
    s_head = (uint8_t)((s_head + 1) % ZB_TX_Q_LEN);
    s_count--;
    xSemaphoreGive(s_mtx);
    return true;
}

static void requeue_front(const zb_tx_item_t *item)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_count >= ZB_TX_Q_LEN) {
        xSemaphoreGive(s_mtx);
        return;
    }
    s_head = (uint8_t)((s_head + ZB_TX_Q_LEN - 1) % ZB_TX_Q_LEN);
    s_q[s_head] = *item;
    s_count++;
    xSemaphoreGive(s_mtx);
}

static void bind_done_cb(esp_zb_zdp_status_t status, void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "Bind response: status=0x%02x%s", status, status == 0 ? "" : " (failed)");
}

static void dispatch_tx(const zb_tx_item_t *item)
{
    esp_zb_zcl_basic_cmd_t basic = {
        .dst_addr_u = { .addr_short = item->short_addr },
        .dst_endpoint = item->endpoint,
        .src_endpoint = HA_GW_ENDPOINT,
    };

    switch (item->kind) {
    case ZB_TX_ON_OFF: {
        esp_zb_zcl_on_off_cmd_t cmd = {
            .zcl_basic_cmd = basic,
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .on_off_cmd_id = item->u8_a,
        };
        uint8_t seq = esp_zb_zcl_on_off_cmd_req(&cmd);
        ESP_LOGI(TAG, "On/Off=%s -> 0x%04x ep=%d seq=%u",
                 item->u8_a == ESP_ZB_ZCL_CMD_ON_OFF_ON_ID ? "ON" : "OFF",
                 item->short_addr, item->endpoint, seq);
        break;
    }
    case ZB_TX_MOVE_TO_LEVEL: {
        esp_zb_zcl_move_to_level_cmd_t cmd = {
            .zcl_basic_cmd = basic,
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .level = item->u8_a,
            .transition_time = 0,
        };
        uint8_t seq = esp_zb_zcl_level_move_to_level_cmd_req(&cmd);
        ESP_LOGI(TAG, "MoveToLevel=%u -> 0x%04x ep=%d seq=%u",
                 item->u8_a, item->short_addr, item->endpoint, seq);
        break;
    }
    case ZB_TX_MOVE_TO_COLOR: {
        esp_zb_zcl_color_move_to_color_cmd_t cmd = {
            .zcl_basic_cmd = basic,
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .color_x = item->u16_a,
            .color_y = item->u16_b,
            .transition_time = 0,
        };
        uint8_t seq = esp_zb_zcl_color_move_to_color_cmd_req(&cmd);
        ESP_LOGI(TAG, "MoveToColor(x=%u,y=%u) -> 0x%04x ep=%d seq=%u",
                 item->u16_a, item->u16_b, item->short_addr, item->endpoint, seq);
        break;
    }
    case ZB_TX_MOVE_TO_HUE_SAT: {
        esp_zb_color_move_to_hue_saturation_cmd_t cmd = {
            .zcl_basic_cmd = basic,
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .hue = item->u8_a,
            .saturation = item->u8_b,
            .transition_time = 0,
        };
        uint8_t seq = esp_zb_zcl_color_move_to_hue_and_saturation_cmd_req(&cmd);
        ESP_LOGI(TAG, "MoveToHueSat(h=%u,s=%u) -> 0x%04x ep=%d seq=%u",
                 item->u8_a, item->u8_b, item->short_addr, item->endpoint, seq);
        break;
    }
    case ZB_TX_MOVE_TO_COLOR_TEMP: {
        esp_zb_zcl_color_move_to_color_temperature_cmd_t cmd = {
            .zcl_basic_cmd = basic,
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .color_temperature = item->u16_a,
            .transition_time = 0,
        };
        uint8_t seq = esp_zb_zcl_color_move_to_color_temperature_cmd_req(&cmd);
        ESP_LOGI(TAG, "MoveToColorTemp=%u -> 0x%04x ep=%d seq=%u",
                 item->u16_a, item->short_addr, item->endpoint, seq);
        break;
    }
    case ZB_TX_WRITE_U16: {
        uint16_t value = item->u16_a;
        esp_zb_zcl_attribute_t attr = {
            .id = item->attr,
            .data = {
                .type = ESP_ZB_ZCL_ATTR_TYPE_U16,
                .size = sizeof(uint16_t),
                .value = &value,
            },
        };
        esp_zb_zcl_write_attr_cmd_t cmd = {
            .zcl_basic_cmd = basic,
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .clusterID = item->cluster,
            .attr_number = 1,
            .attr_field = &attr,
        };
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        esp_zb_zcl_write_attr_cmd_req(&cmd);
        ESP_LOGI(TAG, "Write u16 cluster=0x%04x attr=0x%04x val=%u -> 0x%04x ep=%d",
                 item->cluster, item->attr, item->u16_a, item->short_addr, item->endpoint);
        break;
    }
    case ZB_TX_CONFIG_REPORT: {
        uint16_t change16 = item->u16_a;
        uint8_t change8 = (uint8_t)item->u16_a;
        void *change = NULL;
        if (item->attr_type == ESP_ZB_ZCL_ATTR_TYPE_U16 || item->attr_type == ESP_ZB_ZCL_ATTR_TYPE_S16) {
            change = &change16;
        } else if (item->attr_type != ESP_ZB_ZCL_ATTR_TYPE_BOOL && item->attr_type != 0x30) {
            change = &change8;
        }
        esp_zb_zcl_config_report_record_t record;
        memset(&record, 0, sizeof(record));
        record.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
        record.attributeID = item->attr;
        record.attrType = item->attr_type;
        record.min_interval = item->u16_b;
        record.max_interval = item->max_interval;
        record.reportable_change = change;

        esp_zb_zcl_config_report_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.zcl_basic_cmd = basic;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = item->cluster;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        cmd.record_number = 1;
        cmd.record_field = &record;
        uint8_t seq = esp_zb_zcl_config_report_cmd_req(&cmd);
        ESP_LOGI(TAG, "Config report 0x%04x ep=%d cluster=0x%04x attr=0x%04x seq=%u",
                 item->short_addr, item->endpoint, item->cluster, item->attr, seq);
        break;
    }
    case ZB_TX_BIND: {
        esp_zb_ieee_addr_t dst_ieee;
        esp_zb_get_long_address(dst_ieee);
        esp_zb_zdo_bind_req_param_t bind_req;
        memset(&bind_req, 0, sizeof(bind_req));
        memcpy(bind_req.src_address, item->ieee, sizeof(bind_req.src_address));
        bind_req.src_endp = item->endpoint;
        bind_req.cluster_id = item->cluster;
        bind_req.dst_addr_mode = ESP_ZB_ZDO_BIND_DST_ADDR_MODE_64_BIT_EXTENDED;
        memcpy(bind_req.dst_address_u.addr_long, dst_ieee, sizeof(bind_req.dst_address_u.addr_long));
        bind_req.dst_endp = HA_GW_ENDPOINT;
        bind_req.req_dst_addr = item->short_addr;
        esp_zb_zdo_device_bind_req(&bind_req, bind_done_cb, NULL);
        ESP_LOGI(TAG, "Bind cluster=0x%04x of 0x%04x ep=%d -> gw ep=%d",
                 item->cluster, item->short_addr, item->endpoint, HA_GW_ENDPOINT);
        break;
    }
    case ZB_TX_READ_BASIC: {
        static const uint16_t attr_list[] = {
            ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
            ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
            ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID,
            ZB_BASIC_ATTR_DEVICE_ID,
        };
        esp_zb_zcl_read_attr_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.zcl_basic_cmd = basic;
        cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_BASIC;
        cmd.attr_number = sizeof(attr_list) / sizeof(attr_list[0]);
        cmd.attr_field = (uint16_t *)attr_list;
        cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
        uint8_t seq = esp_zb_zcl_read_attr_cmd_req(&cmd);
        ESP_LOGI(TAG, "Read Basic attrs -> 0x%04x ep=%d seq=%u", item->short_addr, item->endpoint, seq);
        break;
    }
    case ZB_TX_SIMPLE_DESC: {
        esp_zb_zdo_simple_desc_req_param_t req;
        req.addr_of_interest = item->short_addr;
        req.endpoint = item->endpoint;
        esp_zb_zdo_simple_desc_req(&req, zdp_discovery_on_simple_desc,
                                   (void *)(uintptr_t)item->short_addr);
        ESP_LOGI(TAG, "Simple Desc probe ep=%d of 0x%04x", item->endpoint, item->short_addr);
        break;
    }
    default:
        break;
    }
}

static void zb_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_paused) {
            continue;
        }
        zb_tx_item_t item;
        while (dequeue(&item)) {
            if (s_paused) {
                requeue_front(&item);
                break;
            }
            if (!esp_zb_lock_acquire(pdMS_TO_TICKS(ZB_TX_LOCK_WAIT_MS))) {
                ESP_LOGW(TAG, "Zigbee lock timeout, retry kind=%d", (int)item.kind);
                requeue_front(&item);
                vTaskDelay(pdMS_TO_TICKS(50));
                xTaskNotifyGive(s_task);
                break;
            }
            dispatch_tx(&item);
            esp_zb_lock_release();
            vTaskDelay(pdMS_TO_TICKS(ZB_TX_GAP_MS));
        }
    }
}

void zb_tx_init(void)
{
    if (s_task) {
        return;
    }
    s_mtx = xSemaphoreCreateMutex();
    s_head = 0;
    s_count = 0;
    s_paused = false;
    xTaskCreate(zb_tx_task, "zb_tx", 4096, NULL, 4, &s_task);
    ESP_LOGI(TAG, "Zigbee TX queue started (gap=%dms)", ZB_TX_GAP_MS);
}

void zb_tx_pause(void)
{
    s_paused = true;
    if (s_task) {
        vTaskSuspend(s_task);
    }
}

void zb_tx_on_off(uint16_t short_addr, uint8_t endpoint, bool on)
{
    zb_tx_item_t item;
    memset(&item, 0, sizeof(item));
    item.kind = ZB_TX_ON_OFF;
    item.short_addr = short_addr;
    item.endpoint = endpoint;
    item.u8_a = (uint8_t)(on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID);
    enqueue(&item);
}

void zb_tx_move_to_level(uint16_t short_addr, uint8_t endpoint, uint8_t level)
{
    zb_tx_item_t item;
    memset(&item, 0, sizeof(item));
    item.kind = ZB_TX_MOVE_TO_LEVEL;
    item.short_addr = short_addr;
    item.endpoint = endpoint;
    item.u8_a = level;
    enqueue(&item);
}

void zb_tx_move_to_color(uint16_t short_addr, uint8_t endpoint, uint16_t x, uint16_t y)
{
    zb_tx_item_t item;
    memset(&item, 0, sizeof(item));
    item.kind = ZB_TX_MOVE_TO_COLOR;
    item.short_addr = short_addr;
    item.endpoint = endpoint;
    item.u16_a = x;
    item.u16_b = y;
    enqueue(&item);
}

void zb_tx_move_to_hue_sat(uint16_t short_addr, uint8_t endpoint, uint8_t hue, uint8_t sat)
{
    zb_tx_item_t item;
    memset(&item, 0, sizeof(item));
    item.kind = ZB_TX_MOVE_TO_HUE_SAT;
    item.short_addr = short_addr;
    item.endpoint = endpoint;
    item.u8_a = hue;
    item.u8_b = sat;
    enqueue(&item);
}

void zb_tx_move_to_color_temp(uint16_t short_addr, uint8_t endpoint, uint16_t mireds)
{
    zb_tx_item_t item;
    memset(&item, 0, sizeof(item));
    item.kind = ZB_TX_MOVE_TO_COLOR_TEMP;
    item.short_addr = short_addr;
    item.endpoint = endpoint;
    item.u16_a = mireds;
    enqueue(&item);
}

void zb_tx_write_u16(uint16_t short_addr, uint8_t endpoint, uint16_t cluster, uint16_t attr, uint16_t value)
{
    zb_tx_item_t item;
    memset(&item, 0, sizeof(item));
    item.kind = ZB_TX_WRITE_U16;
    item.short_addr = short_addr;
    item.endpoint = endpoint;
    item.cluster = cluster;
    item.attr = attr;
    item.u16_a = value;
    enqueue(&item);
}

void zb_tx_config_report(uint16_t short_addr, uint8_t endpoint, uint16_t cluster, uint16_t attr,
                         uint8_t attr_type, uint16_t change, uint16_t min_interval, uint16_t max_interval)
{
    zb_tx_item_t item;
    memset(&item, 0, sizeof(item));
    item.kind = ZB_TX_CONFIG_REPORT;
    item.short_addr = short_addr;
    item.endpoint = endpoint;
    item.cluster = cluster;
    item.attr = attr;
    item.attr_type = attr_type;
    item.u16_a = change;
    item.u16_b = min_interval;
    item.max_interval = max_interval;
    enqueue(&item);
}

void zb_tx_bind(uint16_t short_addr, uint8_t endpoint, uint16_t cluster, const esp_zb_ieee_addr_t src_ieee)
{
    zb_tx_item_t item;
    memset(&item, 0, sizeof(item));
    item.kind = ZB_TX_BIND;
    item.short_addr = short_addr;
    item.endpoint = endpoint;
    item.cluster = cluster;
    if (src_ieee) {
        memcpy(item.ieee, src_ieee, sizeof(item.ieee));
    }
    enqueue(&item);
}

void zb_tx_read_basic(uint16_t short_addr, uint8_t endpoint)
{
    zb_tx_item_t item;
    memset(&item, 0, sizeof(item));
    item.kind = ZB_TX_READ_BASIC;
    item.short_addr = short_addr;
    item.endpoint = endpoint;
    enqueue(&item);
}

void zb_tx_simple_desc(uint16_t short_addr, uint8_t endpoint)
{
    zb_tx_item_t item;
    memset(&item, 0, sizeof(item));
    item.kind = ZB_TX_SIMPLE_DESC;
    item.short_addr = short_addr;
    item.endpoint = endpoint;
    enqueue(&item);
}

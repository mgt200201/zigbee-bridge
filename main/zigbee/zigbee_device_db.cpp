/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zigbee_device_db.cpp
 *
 * @brief 已知设备数据库 + 桥接端点映射实现
 */

#include "zigbee_device_db.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "zb_db";

/* ── 已知设备列表 ── */
static zigbee_db_device_t s_devices[ZIGBEE_DB_MAX_DEVICES] = {0};

/* ── 桥接端点映射 ── */
#define MAX_BRIDGED_EPS 32

typedef struct {
    bool     in_use;
    uint16_t short_addr;
    uint8_t  zigbee_endpoint;
} ep_map_t;

static ep_map_t s_ep_map[MAX_BRIDGED_EPS] = {0};

/* ==================== 已知设备查询 ==================== */

zigbee_db_device_t *zigbee_db_get_all(void)
{
    return s_devices;
}

zigbee_db_device_t *zigbee_db_find_by_ieee(const esp_zb_ieee_addr_t ieee)
{
    for (int i = 0; i < ZIGBEE_DB_MAX_DEVICES; i++) {
        if (s_devices[i].in_use &&
            memcmp(s_devices[i].ieee_addr, ieee, sizeof(esp_zb_ieee_addr_t)) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

zigbee_db_device_t *zigbee_db_find_by_short(uint16_t short_addr)
{
    for (int i = 0; i < ZIGBEE_DB_MAX_DEVICES; i++) {
        if (s_devices[i].in_use && s_devices[i].short_addr == short_addr) {
            return &s_devices[i];
        }
    }
    return NULL;
}

void zigbee_db_add_or_update(const esp_zb_ieee_addr_t ieee, uint16_t short_addr)
{
    /* 1. 按 IEEE 查找 → 更新 short_addr */
    zigbee_db_device_t *dev = zigbee_db_find_by_ieee(ieee);
    if (dev) {
        if (dev->short_addr != short_addr) {
            ESP_LOGI(TAG, "IEEE known: short 0x%04x -> 0x%04x", dev->short_addr, short_addr);
            /* 同步端点映射 */
            for (int i = 0; i < MAX_BRIDGED_EPS; i++) {
                if (s_ep_map[i].in_use && s_ep_map[i].short_addr == dev->short_addr) {
                    s_ep_map[i].short_addr = short_addr;
                }
            }
            dev->short_addr = short_addr;
        }
        return;
    }

    /* 2. 按 short_addr 查找 → 更新 IEEE */
    dev = zigbee_db_find_by_short(short_addr);
    if (dev) {
        memcpy(dev->ieee_addr, ieee, sizeof(esp_zb_ieee_addr_t));
        return;
    }

    /* 3. 插入新条目 */
    for (int i = 0; i < ZIGBEE_DB_MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) {
            s_devices[i].in_use = true;
            memcpy(s_devices[i].ieee_addr, ieee, sizeof(esp_zb_ieee_addr_t));
            s_devices[i].short_addr = short_addr;
            ESP_LOGI(TAG, "New device: short=0x%04x [%d/%d]", short_addr, i + 1, ZIGBEE_DB_MAX_DEVICES);
            return;
        }
    }

    ESP_LOGW(TAG, "Device DB full (%d), dropped 0x%04x", ZIGBEE_DB_MAX_DEVICES, short_addr);
}

/* ==================== 桥接端点映射 ==================== */

void zigbee_db_register_ep(uint16_t short_addr, uint8_t endpoint)
{
    for (int i = 0; i < MAX_BRIDGED_EPS; i++) {
        if (!s_ep_map[i].in_use) {
            s_ep_map[i].in_use = true;
            s_ep_map[i].short_addr = short_addr;
            s_ep_map[i].zigbee_endpoint = endpoint;
            ESP_LOGI(TAG, "EP map: short=0x%04x <-> ep=%d [%d/%d]",
                     short_addr, endpoint, i + 1, MAX_BRIDGED_EPS);
            return;
        }
    }
    ESP_LOGW(TAG, "EP map full (%d)", MAX_BRIDGED_EPS);
}

uint16_t zigbee_db_short_by_ep(uint8_t endpoint)
{
    for (int i = 0; i < MAX_BRIDGED_EPS; i++) {
        if (s_ep_map[i].in_use && s_ep_map[i].zigbee_endpoint == endpoint) {
            return s_ep_map[i].short_addr;
        }
    }
    return 0xFFFF;
}

uint8_t zigbee_db_ep_by_short(uint16_t short_addr)
{
    for (int i = 0; i < MAX_BRIDGED_EPS; i++) {
        if (s_ep_map[i].in_use && s_ep_map[i].short_addr == short_addr) {
            return s_ep_map[i].zigbee_endpoint;
        }
    }
    return 0xFF;
}
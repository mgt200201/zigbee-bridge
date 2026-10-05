/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zigbee_device_db.h
 *
 * @brief Zigbee 已知设备数据库 + 桥接端点映射
 *
 * 职责：
 *   - 已知设备列表（IEEE / short_addr 双向查找）
 *   - 设备入网/离网的增删改
 *   - 桥接端点映射（short_addr ↔ endpoint）
 *
 * 原属于 zboss_manager.cpp，按大厂单一职责原则拆分。
 */

#pragma once

#include <esp_err.h>
#include <esp_zigbee_core.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 已知设备类型 ==================== */

#define ZIGBEE_DB_MAX_DEVICES 16

typedef struct {
    bool in_use;
    esp_zb_ieee_addr_t ieee_addr;
    uint16_t short_addr;
} zigbee_db_device_t;

/* ==================== 已知设备查询 ==================== */

zigbee_db_device_t *zigbee_db_get_all(void);

zigbee_db_device_t *zigbee_db_find_by_ieee(const esp_zb_ieee_addr_t ieee);

zigbee_db_device_t *zigbee_db_find_by_short(uint16_t short_addr);

/**
 * @brief 添加或更新已知设备
 *
 * 优先级：
 *   1. 按 IEEE 查找 → 更新 short_addr
 *   2. 按 short_addr 查找 → 更新 IEEE
 *   3. 两者都找不到 → 插入新条目
 *
 * 会自动同步端点映射表中的 short_addr。
 */
void zigbee_db_add_or_update(const esp_zb_ieee_addr_t ieee, uint16_t short_addr);

/** Remove by IEEE (preferred) and/or short address; also drops endpoint map rows. */
void zigbee_db_remove(const esp_zb_ieee_addr_t ieee, uint16_t short_addr);

/* ==================== 桥接端点映射 ==================== */

void zigbee_db_register_ep(uint16_t short_addr, uint8_t endpoint);

uint16_t zigbee_db_short_by_ep(uint8_t endpoint);

uint8_t zigbee_db_ep_by_short(uint16_t short_addr);

#ifdef __cplusplus
}
#endif
/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zigbee_matter_map.h
 *
 * @brief Zigbee → Matter 集群/属性映射表
 *
 * 表驱动设计：新增传感器类型只需在 s_sensor_map[] 中添加一行，
 * 无需修改 bridge_manager_report_sensor_data 的分支逻辑。
 */

#pragma once

#include <esp_matter_attribute_utils.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 映射表条目 ==================== */

/**
 * @brief Zigbee → Matter 传感器映射条目
 *
 * 每个条目描述一个 Zigbee 集群如何映射到 Matter 集群 + 属性。
 */
typedef struct {
    uint16_t zigbee_cluster;       /**< Zigbee 集群 ID (e.g. 0x0402) */
    uint32_t matter_cluster;       /**< Matter 集群 ID (e.g. TemperatureMeasurement::Id) */
    uint32_t matter_attr;          /**< Matter 属性 ID */
    esp_matter_val_type_t val_type; /**< 数值类型 (INT16/UINT16/...) */
} zigbee_matter_map_entry_t;

/* ==================== 查询接口 ==================== */

/**
 * @brief 查找 Zigbee 集群对应的 Matter 映射
 * @param zigbee_cluster  Zigbee 集群 ID
 * @return 找到返回映射条目指针，否则返回 NULL
 */
const zigbee_matter_map_entry_t *zigbee_matter_map_find(uint16_t zigbee_cluster);

/**
 * @return 映射表条目数量（供调试使用）
 */
int zigbee_matter_map_count(void);

#ifdef __cplusplus
}
#endif
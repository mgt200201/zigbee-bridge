/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zigbee_matter_map.cpp
 *
 * @brief Zigbee → Matter 传感器映射表实现
 *
 * 新增传感器类型只需在 s_sensor_map[] 中添加一行。
 * 所有 Matter 集群 ID 使用 Chip 命名空间常数。
 */

#include "zigbee_matter_map.h"

#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Attributes.h>

using namespace chip::app::Clusters;

static const zigbee_matter_map_entry_t s_sensor_map[] = {
    {
        .zigbee_cluster = 0x0402,  /* Temperature Measurement */
        .matter_cluster = TemperatureMeasurement::Id,
        .matter_attr    = TemperatureMeasurement::Attributes::MeasuredValue::Id,
        .val_type       = ESP_MATTER_VAL_TYPE_NULLABLE_INT16,
    },
    {
        .zigbee_cluster = 0x0405,  /* Relative Humidity */
        .matter_cluster = RelativeHumidityMeasurement::Id,
        .matter_attr    = RelativeHumidityMeasurement::Attributes::MeasuredValue::Id,
        .val_type       = ESP_MATTER_VAL_TYPE_NULLABLE_UINT16,
    },
    /* ── 在此添加新传感器类型 ── */
    /* {
     *     .zigbee_cluster = 0x0400,  // Illuminance Measurement
     *     .matter_cluster = IlluminanceMeasurement::Id,
     *     .matter_attr    = IlluminanceMeasurement::Attributes::MeasuredValue::Id,
     *     .val_type       = ESP_MATTER_VAL_TYPE_NULLABLE_UINT16,
     * },
     */
};

static const int s_sensor_map_size = sizeof(s_sensor_map) / sizeof(s_sensor_map[0]);

const zigbee_matter_map_entry_t *zigbee_matter_map_find(uint16_t zigbee_cluster)
{
    for (int i = 0; i < s_sensor_map_size; i++) {
        if (s_sensor_map[i].zigbee_cluster == zigbee_cluster) {
            return &s_sensor_map[i];
        }
    }
    return NULL;
}

int zigbee_matter_map_count(void)
{
    return s_sensor_map_size;
}
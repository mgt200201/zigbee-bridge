/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_matter_attribute_utils.h>
#include <ha/esp_zigbee_ha_standard.h>
#include <esp_zigbee_core.h>
#include <app_bridged_device.h>
#include <stdint.h>

#define ESP_MATTER_TEMP_HUMIDITY_SENSOR_DEVICE_TYPE_ID 0x0310

void zigbee_bridge_find_temp_humidity_sensor_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr, uint8_t endpoint, void *user_ctx);

void zigbee_bridge_report_temp_humidity_to_matter(uint16_t short_addr, uint8_t endpoint,
                                                  uint16_t cluster_id, int16_t raw_value);

esp_err_t zigbee_bridge_attribute_update(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, app_bridged_device_t *zigbee_device);
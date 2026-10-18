/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file handler_ota.cpp
 *
 * @brief OTA Upgrade cluster handler (0x0019)
 *
 * 设备入网/重连后，网关主动读取 CurrentFileVersion(0x0002)，
 * 用于确认子设备当前实际运行的固件版本。
 */

#include "handler_ota.h"
#include <esp_log.h>
#include <string.h>
#include <zcl/esp_zigbee_zcl_ota.h>

static const char *TAG = "ota_ver";

static esp_err_t ota_read_attr_resp(const esp_zb_zcl_cmd_read_attr_resp_message_t *msg)
{
    if (!msg) {
        return ESP_FAIL;
    }
    uint16_t short_addr = msg->info.src_address.u.short_addr;
    if (short_addr == 0xFFFF) {
        return ESP_ERR_NOT_FOUND;
    }

    for (esp_zb_zcl_read_attr_resp_variable_t *var = msg->variables; var; var = var->next) {
        if (var->attribute.id != ESP_ZB_ZCL_ATTR_OTA_UPGRADE_FILE_VERSION_ID) {
            continue;
        }
        if (var->status != ESP_ZB_ZCL_STATUS_SUCCESS || var->attribute.data.value == NULL) {
            ESP_LOGW(TAG, "0x%04x: read version failed, status=0x%02x", short_addr, var->status);
            continue;
        }
        if (var->attribute.data.size < 4) {
            ESP_LOGW(TAG, "0x%04x: version size=%u, expected 4",
                     short_addr, (unsigned)var->attribute.data.size);
            continue;
        }
        uint32_t version = 0;
        memcpy(&version, var->attribute.data.value, sizeof(version));
        ESP_LOGI(TAG, "0x%04x ep=%d: current file version = 0x%08lX (%lu)",
                 short_addr, msg->info.src_endpoint,
                 (unsigned long)version, (unsigned long)version);
    }

    return ESP_OK;
}

const zcl_device_handler_t ota_upgrade_handler = {
    .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE,
    .name = "ota_upgrade",
    .on_read_attr_resp = ota_read_attr_resp,
};

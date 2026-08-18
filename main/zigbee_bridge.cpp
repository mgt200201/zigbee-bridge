/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app_bridged_device.h>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_bridge.h>

#include <zigbee_bridge.h>

extern "C" void app_zboss_register_bridged_device(uint16_t short_addr, uint8_t zigbee_endpoint);

static const char *TAG = "zigbee_bridge";

using namespace chip::app::Clusters;
using namespace esp_matter;
using namespace esp_matter::cluster;

extern uint16_t aggregator_endpoint_id;

void zigbee_bridge_find_temp_humidity_sensor_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr, uint8_t endpoint, void *user_ctx)
{
    ESP_LOGI(TAG, "temp_humidity_sensor found: address:0x%04" PRIx16 ", endpoint:%" PRId8 ", response_status:%d", addr, endpoint, zdo_status);
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        node_t *node = node::get();
        if (!node) {
            ESP_LOGE(TAG, "Could not find esp_matter node");
            return;
        }
        if (app_bridge_get_device_by_zigbee_shortaddr(addr)) {
            ESP_LOGI(TAG, "Bridged node for 0x%04" PRIx16 " zigbee device on endpoint %" PRId16 " has been created", addr,
                     app_bridge_get_matter_endpointid_by_zigbee_shortaddr(addr));
        } else {
            app_bridged_device_t *bridged_device =
                app_bridge_create_bridged_device(node, aggregator_endpoint_id, ESP_MATTER_TEMP_HUMIDITY_SENSOR_DEVICE_TYPE_ID,
                                                 ESP_MATTER_BRIDGED_DEVICE_TYPE_ZIGBEE,
                                                 app_bridge_zigbee_address(endpoint, addr), NULL);
            if (!bridged_device) {
                ESP_LOGE(TAG, "Failed to create zigbee bridged device (temp_humidity sensor)");
                return;
            }
            ESP_LOGI(TAG, "Create/Update bridged node for 0x%04" PRIx16 " zigbee device on endpoint %" PRId16 "", addr,
                     app_bridge_get_matter_endpointid_by_zigbee_shortaddr(addr));
            app_zboss_register_bridged_device(addr, endpoint);
        }
    }
}

void zigbee_bridge_report_temp_humidity_to_matter(uint16_t short_addr, uint8_t endpoint,
                                                   uint16_t cluster_id, int16_t raw_value)
{
    app_bridged_device_t *zigbee_device = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (!zigbee_device || !zigbee_device->dev || !zigbee_device->dev->endpoint) {
        ESP_LOGW(TAG, "Bridged device not found for short_addr=0x%04x, endpoint=%d", short_addr, endpoint);
        return;
    }

    uint16_t matter_ep = esp_matter::endpoint::get_id(zigbee_device->dev->endpoint);
    uint32_t cluster_id_matter = 0;
    uint32_t attr_id = 0;

    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT) {
        cluster_id_matter = TemperatureMeasurement::Id;
        attr_id = TemperatureMeasurement::Attributes::MeasuredValue::Id;
        ESP_LOGI(TAG, "Reporting temperature: raw=%d, matter_ep=%d", raw_value, matter_ep);
    } else if (cluster_id == 0x0405) {
        cluster_id_matter = RelativeHumidityMeasurement::Id;
        attr_id = RelativeHumidityMeasurement::Attributes::MeasuredValue::Id;
        ESP_LOGI(TAG, "Reporting humidity: raw=%d, matter_ep=%d", raw_value, matter_ep);
    } else {
        return;
    }

    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT) {
        val.type = ESP_MATTER_VAL_TYPE_NULLABLE_INT16;
        val.val.i16 = raw_value;
    } else {
        val.type = ESP_MATTER_VAL_TYPE_NULLABLE_UINT16;
        val.val.u16 = (uint16_t)raw_value;
    }

    esp_err_t err = attribute::update(matter_ep, cluster_id_matter, attr_id, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "attribute::update failed: %s", esp_err_to_name(err));
    }
}

esp_err_t zigbee_bridge_attribute_update(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, app_bridged_device_t *zigbee_device)
{
    if (!zigbee_device || !zigbee_device->dev || !zigbee_device->dev->endpoint) {
        ESP_LOGE(TAG, "Unable to Update Bridge Device, ep: %" PRId16 ", cluster: %" PRId32 ", att: %" PRId32 "",
                 endpoint_id, cluster_id, attribute_id);
        return ESP_OK;
    }
    return ESP_OK;
}
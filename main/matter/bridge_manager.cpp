/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file bridge_manager.cpp
 *
 * @brief Matter 桥接管理层
 *
 * 负责：
 * - Matter 桥接设备的创建和删除
 * - Zigbee ↔ Matter 属性翻译
 * - 设备标识信息的注册和持久化
 * - 传感器数据上报
 *
 * 本层只懂 Matter 协议，所有 Zigbee 细节通过 zdp_discovery 接口访问。
 */

#include <bridge_manager.h>
#include "device_info.h"
#include "zigbee_matter_map.h"
#include <commissioning_mgr.h>
#include <zboss_manager.h>
#include <zdp_discovery.h>
#include <zcl_bridge_api.h>
#include <wifi_manager.h>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_bridge.h>
#include <esp_matter_console.h>
#include <nvs_flash.h>

#include <setup_payload/SetupPayload.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>

#include <common_macros.h>

static const char *TAG = "bridge_manager";

using namespace chip::app::Clusters;
using namespace esp_matter;
using namespace esp_matter::cluster;

/** Aggregator 端点 ID */
static uint16_t s_aggregator_ep_id = chip::kInvalidEndpointId;

/* ==================== Bridge API 适配层（依赖注入） ==================== */

/**
 * 将 bridge_manager 的全局函数包装为 zcl_bridge_api 接口，
 * 使 ZCL handler 层不直接依赖 bridge_manager，而是通过接口注入。
 */

static void bridge_api_report_sensor(uint16_t short_addr, uint8_t endpoint,
                                     uint16_t cluster_id, int16_t raw_value)
{
    bridge_manager_report_sensor_data(short_addr, endpoint, cluster_id, raw_value);
}

static void bridge_api_report_on_off(uint16_t short_addr, uint8_t endpoint, bool on)
{
    app_bridged_device_t *dev = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (!dev || !dev->dev || !dev->dev->endpoint) {
        ESP_LOGW(TAG, "Cannot report On/Off: no bridge for 0x%04x", short_addr);
        return;
    }
    uint16_t matter_ep = esp_matter::endpoint::get_id(dev->dev->endpoint);
    esp_matter_attr_val_t val = esp_matter_bool(on);
    esp_err_t err = attribute::update(matter_ep, OnOff::Id,
                                      OnOff::Attributes::OnOff::Id, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "On/Off attribute::update failed: %s", esp_err_to_name(err));
    }
}

static bool bridge_api_is_bridged(uint16_t short_addr)
{
    return bridge_manager_is_device_bridged(short_addr);
}

static uint8_t bridge_api_get_ep(uint16_t short_addr)
{
    return bridge_manager_get_endpoint_by_short(short_addr);
}

/** Bridge API 实例（静态全局，在 init 中注册到 ZCL 层） */
static const zcl_bridge_api_t s_bridge_api_impl = {
    .report_sensor = bridge_api_report_sensor,
    .report_on_off = bridge_api_report_on_off,
    .is_device_bridged = bridge_api_is_bridged,
    .get_endpoint_by_short = bridge_api_get_ep,
};

void bridge_manager_init(uint16_t aggregator_endpoint_id)
{
    s_aggregator_ep_id = aggregator_endpoint_id;

    /* 注册桥接接口到 ZCL 层（依赖注入） */
    zcl_bridge_api_register(&s_bridge_api_impl);

    ESP_LOGI(TAG, "Bridge manager initialized, aggregator endpoint=%d", aggregator_endpoint_id);
}

/* ==================== Matter 事件回调 ==================== */

void bridge_manager_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
    case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
        /* WiFi/网络事件委托给 wifi_manager 处理 */
        wifi_manager_handle_chip_event(event, arg);
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        {
            chip::PayloadContents payloadContents;
            payloadContents.setUpPINCode = commissioning_mgr_get_pin();
            payloadContents.discriminator.SetLongValue(commissioning_mgr_get_discriminator());
            payloadContents.rendezvousInformation.SetValue(chip::RendezvousInformationFlag::kBLE);
            payloadContents.commissioningFlow = chip::CommissioningFlow::kStandard;
            std::string manualPairingCode;
            chip::ManualSetupPayloadGenerator generator(payloadContents);
            generator.payloadDecimalStringRepresentation(manualPairingCode);
            ESP_LOGI(TAG, "Manual Pairing Code: %s", manualPairingCode.c_str());
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    default:
        break;
    }
}

/* ==================== 桥接端点创建回调 ==================== */

esp_err_t bridge_manager_create_devices_cb(esp_matter::endpoint_t *ep, uint32_t device_type_id, void *priv_data)
{
    (void)priv_data;
    switch (device_type_id) {
    case ESP_MATTER_TEMP_HUMIDITY_SENSOR_DEVICE_TYPE_ID: {
        temperature_measurement::config_t temp_conf;
        temp_conf.measured_value = { static_cast<int16_t>(0) };
        temp_conf.min_measured_value = { static_cast<int16_t>(-2000) };
        temp_conf.max_measured_value = { static_cast<int16_t>(2000) };
        temperature_measurement::create(ep, &temp_conf, CLUSTER_FLAG_SERVER);
        relative_humidity_measurement::config_t humidity_conf;
        humidity_conf.measured_value = { static_cast<uint16_t>(0) };
        humidity_conf.min_measured_value = { static_cast<uint16_t>(0) };
        humidity_conf.max_measured_value = { static_cast<uint16_t>(10000) };
        relative_humidity_measurement::create(ep, &humidity_conf, CLUSTER_FLAG_SERVER);
        break;
    }
    default:
        ESP_LOGE(TAG, "Unsupported bridged matter device type: 0x%04x", device_type_id);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ==================== 属性更新回调 ==================== */

esp_err_t bridge_manager_attr_update_cb(esp_matter::attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == esp_matter::attribute::PRE_UPDATE) {
        return bridge_manager_attr_update(endpoint_id, cluster_id, attribute_id,
                                           val, (app_bridged_device_t *)priv_data);
    }
    return ESP_OK;
}

/* ==================== 桥接设备查询 ==================== */

bool bridge_manager_is_device_bridged(uint16_t short_addr)
{
    return app_bridge_get_device_by_zigbee_shortaddr(short_addr) != NULL;
}

uint8_t bridge_manager_get_endpoint_by_short(uint16_t short_addr)
{
    /* 优先从 device_info 查找（内存中的在线状态） */
    device_info_t *info = device_info_find_by_short_addr(short_addr);
    if (info) {
        return info->zigbee_endpoint;
    }
    /* 回退：从 Matter bridge 已恢复的数据中获取 Zigbee 端点号
     * 重启后 device_info.short_addr = 0xFFFF，但 Matter bridge 的 zigbee 地址已从 NVS 恢复 */
    app_bridged_device_t *bridged = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (bridged) {
        ESP_LOGI(TAG, "Fell back to Matter bridge for endpoint: short=0x%04hx, ep=%d",
                 short_addr, bridged->dev_addr.zigbee_endpointid);
        return bridged->dev_addr.zigbee_endpointid;
    }
    return 0xFF;
}

void bridge_manager_on_device_discovered(uint16_t short_addr, uint8_t endpoint)
{
    /* 检查是否已存在桥接设备 */
    if (app_bridge_get_device_by_zigbee_shortaddr(short_addr)) {
        ESP_LOGI(TAG, "Bridged node for 0x%04x already exists on endpoint %d",
                 short_addr, app_bridge_get_matter_endpointid_by_zigbee_shortaddr(short_addr));
        return;
    }

    /* 创建 Matter 桥接设备 */
    node_t *node = node::get();
    if (!node) {
        ESP_LOGE(TAG, "Could not find esp_matter node");
        return;
    }

    app_bridged_device_t *bridged_device =
        app_bridge_create_bridged_device(node, s_aggregator_ep_id,
                                         ESP_MATTER_TEMP_HUMIDITY_SENSOR_DEVICE_TYPE_ID,
                                         ESP_MATTER_BRIDGED_DEVICE_TYPE_ZIGBEE,
                                         app_bridge_zigbee_address(endpoint, short_addr), NULL);
    if (!bridged_device) {
        ESP_LOGE(TAG, "Failed to create bridged device for 0x%04x", short_addr);
        return;
    }

    uint16_t matter_ep = app_bridge_get_matter_endpointid_by_zigbee_shortaddr(short_addr);
    ESP_LOGI(TAG, "Created bridged device for 0x%04x on Matter endpoint %d", short_addr, matter_ep);

    /* 注册到 zboss_manager */
    zboss_manager_register_bridged_device(short_addr, endpoint);

    /* 主动读取 Basic 属性 */
    zdp_discovery_read_basic_attributes(short_addr, endpoint);
}

void bridge_manager_on_device_identified(uint32_t device_id, uint16_t uiid,
                                          uint16_t short_addr, uint8_t endpoint,
                                          const esp_zb_ieee_addr_t ieee_addr)
{
    device_info_t *info = device_info_find_by_device_id(device_id);

    if (info) {
        /* ===== CASE 1: 设备重连 ===== */
        uint16_t old_short_addr = info->short_addr;
        device_info_update_online(device_id, short_addr, endpoint, ieee_addr);

        if (old_short_addr != short_addr && info->matter_endpoint_id != 0) {
            app_bridged_device_t *bridged = app_bridge_get_device_by_zigbee_shortaddr(old_short_addr);
            if (bridged) {
                bridged->dev_addr.zigbee_shortaddr = short_addr;
                ESP_LOGI(TAG, "Updated Matter bridge addr: 0x%04hx -> 0x%04hx (device_id=0x%08X)",
                         old_short_addr, short_addr, (unsigned int)device_id);
            }
            zboss_manager_register_bridged_device(short_addr, endpoint);
        }

        if (info->uiid == UIID_UNKNOWN && uiid != UIID_UNKNOWN) {
            info->uiid = uiid;
        }

        ESP_LOGI(TAG, "Device reconnected: device_id=0x%08X, Matter ep=%d",
                 (unsigned int)device_id, info->matter_endpoint_id);
        return;
    }

    /* ===== CASE 2: 新设备 ===== */
    app_bridged_device_t *bridged = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (bridged) {
        uint16_t matter_ep = app_bridge_get_matter_endpointid_by_zigbee_shortaddr(short_addr);
        device_info_t *new_info = device_info_register(device_id, uiid, short_addr, endpoint, ieee_addr);
        if (new_info) {
            device_info_set_matter_endpoint(device_id, matter_ep);
        }
        ESP_LOGI(TAG, "New device associated: device_id=0x%08X, Matter ep=%d, zigbee=0x%04hx",
                 (unsigned int)device_id, matter_ep, short_addr);
    } else {
        /* 防御性处理：创建桥接设备 */
        ESP_LOGW(TAG, "No bridge for short_addr=0x%04hx (device_id=0x%08X), creating now",
                 short_addr, (unsigned int)device_id);
        node_t *node = node::get();
        if (node) {
            bridged = app_bridge_create_bridged_device(node, s_aggregator_ep_id,
                                                       ESP_MATTER_TEMP_HUMIDITY_SENSOR_DEVICE_TYPE_ID,
                                                       ESP_MATTER_BRIDGED_DEVICE_TYPE_ZIGBEE,
                                                       app_bridge_zigbee_address(endpoint, short_addr), NULL);
            if (bridged) {
                uint16_t matter_ep = app_bridge_get_matter_endpointid_by_zigbee_shortaddr(short_addr);
                device_info_t *new_info = device_info_register(device_id, uiid, short_addr, endpoint, ieee_addr);
                if (new_info) {
                    device_info_set_matter_endpoint(device_id, matter_ep);
                }
                zboss_manager_register_bridged_device(short_addr, endpoint);
                ESP_LOGI(TAG, "Bridge created (deferred) for device_id=0x%08X on Matter ep %d",
                         (unsigned int)device_id, matter_ep);
            }
        }
    }

    zdp_discovery_send_register_status(short_addr, endpoint, 129);
}

void bridge_manager_report_sensor_data(uint16_t short_addr, uint8_t endpoint,
                                       uint16_t cluster_id, int16_t raw_value)
{
    app_bridged_device_t *zigbee_device = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (!zigbee_device || !zigbee_device->dev || !zigbee_device->dev->endpoint) {
        ESP_LOGW(TAG, "Bridged device not found for short_addr=0x%04x, ep=%d", short_addr, endpoint);
        return;
    }

    const zigbee_matter_map_entry_t *entry = zigbee_matter_map_find(cluster_id);
    if (!entry) {
        ESP_LOGD(TAG, "No Matter mapping for zigbee cluster 0x%04x (short=0x%04x)", cluster_id, short_addr);
        return;
    }

    uint16_t matter_ep = esp_matter::endpoint::get_id(zigbee_device->dev->endpoint);

    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    val.type = entry->val_type;
    if (entry->val_type == ESP_MATTER_VAL_TYPE_NULLABLE_INT16) {
        val.val.i16 = raw_value;
    } else if (entry->val_type == ESP_MATTER_VAL_TYPE_NULLABLE_UINT16) {
        val.val.u16 = (uint16_t)raw_value;
    }

    esp_err_t err = attribute::update(matter_ep, entry->matter_cluster, entry->matter_attr, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "attribute::update failed (cluster=0x%04x, ep=%d): %s",
                 cluster_id, matter_ep, esp_err_to_name(err));
    }
}

esp_err_t bridge_manager_attr_update(uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id,
                                      esp_matter_attr_val_t *val,
                                      app_bridged_device_t *zigbee_device)
{
    if (!zigbee_device || !zigbee_device->dev || !zigbee_device->dev->endpoint) {
        ESP_LOGE(TAG, "Unable to update bridge device, ep=%" PRIu16 ", cluster=%" PRIu32 ", attr=%" PRIu32 "",
                 endpoint_id, cluster_id, attribute_id);
        return ESP_OK;
    }
    /* TODO: 实现 Matter → Zigbee 的写入转发 */
    return ESP_OK;
}
/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file wifi_manager.cpp
 *
 * @brief WiFi 连接管理层
 *
 * 负责监测 WiFi 连接状态，处理断线重连。
 * 通过 Chip DeviceEvent 机制接收事件通知。
 *
 * 依赖：Matter 协议栈已启动（由 app_main 保证）
 */

#include "wifi_manager.h"
#include "zb_device_ota.h"
#include <esp_log.h>
#include <esp_wifi.h>

#include <esp_matter.h>

static const char *TAG = "wifi_manager";

/* ==================== 生命周期 ==================== */

void wifi_manager_init(void)
{
    ESP_LOGI(TAG, "WiFi manager initialized");
}

/* ==================== 事件处理 ==================== */

void wifi_manager_handle_chip_event(const void *event, intptr_t arg)
{
    (void)arg;
    const ChipDeviceEvent *chip_event = static_cast<const ChipDeviceEvent *>(event);

    switch (chip_event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address Changed");
        /* HA 经局域网 HTTP 上传子设备 OTA 包 */
        if (zb_device_ota_http_start() != ESP_OK) {
            ESP_LOGW(TAG, "zb_device_ota HTTP start deferred/failed");
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
        if (chip_event->WiFiConnectivityChange.Result
            == chip::DeviceLayer::kConnectivity_Established) {
            ESP_LOGI(TAG, "WiFi connected");
        } else if (chip_event->WiFiConnectivityChange.Result
                   == chip::DeviceLayer::kConnectivity_Lost) {
            ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
            esp_wifi_connect();
        }
        break;

    default:
        break;
    }
}
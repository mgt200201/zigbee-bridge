/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file app_main.cpp
 *
 * @brief 应用入口——纯初始化编排
 *
 * 本文件只做一件事：按照正确的顺序初始化各模块。
 * 所有具体业务逻辑已被分解到对应的模块中：
 *   - Matter/Chip 事件处理 →  matter/bridge_manager.cpp
 *   - Zigbee 网络管理层    →  zigbee/zboss_manager.cpp
 *   - 外设驱动 (按键/LED)  →  peripherals/button.cpp, led.cpp
 *
 * 本文件仅包含 main() 函数和必要的跨层回调注册（lambda 表达式）。
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_system.h>

#include <esp_matter.h>
#include <esp_matter_console.h>

#include <common_macros.h>
#include <app_bridged_device.h>

#include <zboss_manager.h>
#include <zdp_discovery.h>
#include <bridge_manager.h>
#include <device_info.h>
#include <wifi_manager.h>
#include <ota_manager.h>
#include <commissioning_mgr.h>
#include <button.h>
#include <led.h>
#include "build_info.h"
#include "self_test.h"
#include "watchdog.h"

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::endpoint;

/* ==================== 主函数 ==================== */

extern "C" void app_main(void)
{
    /* ---- 第 0 层：构建元信息 + 启动自检 ---- */
    ESP_LOGI(TAG, "Firmware: %s", build_info_get_string());
    ESP_LOGI(TAG, "ESP-IDF: %d.%d", ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR);

    esp_err_t self_test_ret = self_test_run();
    if (self_test_ret != ESP_OK) {
        ESP_LOGW(TAG, "Self-test returned %s, continuing in degraded mode",
                 esp_err_to_name(self_test_ret));
    }

    /* ---- 看门狗初始化（监视所有长运行任务） ---- */
    ESP_ERROR_CHECK(watchdog_init());

    /* ---- 第 1 层：基础硬件初始化 ---- */
    nvs_flash_init();
    ESP_LOGI(TAG, "NVS initialized");

    commissioning_mgr_init();
    ESP_LOGI(TAG, "Commissioning credentials initialized");

    button_init(GPIO_NUM_9);
    led_init(GPIO_NUM_10, false); /* active_low=false: 高电平点亮 */
    ESP_LOGI(TAG, "Peripherals initialized");

    /* ---- 第 2 层：Matter 协议栈初始化 ---- */
    device_info_init();

    node::config_t node_config;
    node_t *node = node::create(&node_config, bridge_manager_attr_update_cb, NULL);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    aggregator::config_t aggregator_config;
    endpoint_t *aggregator = endpoint::aggregator::create(node, &aggregator_config,
                                                          ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(aggregator != nullptr, ESP_LOGE(TAG, "Failed to create aggregator endpoint"));

    uint16_t aggregator_endpoint_id = endpoint::get_id(aggregator);

    esp_err_t err = esp_matter::start(bridge_manager_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    err = app_bridge_initialize(node, bridge_manager_create_devices_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to resume bridged endpoints: %d", err));

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
#endif

    bridge_manager_init(aggregator_endpoint_id);

    /* WiFi 管理层初始化（处理断线重连） */
    wifi_manager_init();

    /* OTA 固件升级初始化 */
    ota_manager_init();
    ota_manager_start_periodic_check();

    /* ---- 第 3 层：Zigbee 协议栈启动 ---- */
    zdp_discovery_init();

    /* 注册 Steering ↔ LED 联动回调（zboss_manager 内部定时检查超时自动关闭） */
    zboss_manager_register_steering_callback([](bool active) {
        if (active) {
            led_blink_start(500);
        } else {
            led_blink_stop();
            led_off();
        }
    });
    ESP_LOGI(TAG, "Steering LED callback registered");

    zboss_manager_start();

    /* ---- 第 4 层：按键事件路由 ---- */
    button_register_callback([](button_event_t event, void *user_data) {
        (void)user_data;
        if (event == BUTTON_EVENT_SHORT_PRESS) {
            ESP_LOGI(TAG, "Button short press");
            zboss_manager_handle_short_press();
        } else if (event == BUTTON_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "Button long press, restarting...");
            esp_restart();
        }
    }, NULL);
    ESP_LOGI(TAG, "Button callback registered");

    ESP_LOGI(TAG, "=== System initialized ===");

    /* 主任务即将退出，从看门狗中移除 */
    watchdog_remove_self();
}
/*
   SPDX-License-Identifier: Apache-2.0

   应用初始化编排实现。

   本文件按"启动阶段"组织初始化代码，每个阶段只做一类事情：
     第 0 层  构建信息 / 自检 / 看门狗 / NVS / 配网凭据
     第 1 层  外设（H2 保持运行、按键、LED、外部 RTC）
     第 2 层  Matter 协议栈与桥接器
     第 3 层  运行期服务（桥接管理、WiFi、OTA）
     第 4 层  Zigbee 协议栈
     第 5 层  串口命令与按键事件
   具体业务逻辑仍在各自模块中（matter/ zigbee/ provisioning/ peripherals/）。
*/

#include "app_init.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>

#include <esp_matter.h>

#include <app_bridged_device.h>
#include <common_macros.h>

#include <app/server/Server.h>

#include <matter/app_status.h>
#include <bridge_manager.h>
#include <button.h>
#include <commissioning_mgr.h>
#include <device_info.h>
#include <device_instance_provider.h>
#include <matter_ota.h>
#include <ota_manager.h>
#include <rcp_coprocessor.h>
#include <rtc_rx8025t.h>
#include <rtc_time_diag.h>
#include <serial_cmd.h>
#include <status_led.h>
#include <wifi_manager.h>
#include <zb_device_ota.h>
#include <zboss_manager.h>
#include <zdp_discovery.h>

#include "board_config.h"
#include "build_info.h"
#include "commissioning_sync.h"
#include "self_test.h"
#include "watchdog.h"

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::endpoint;

/* ==================== 第 0 层：基础环境 ==================== */

extern "C" void app_init_early(void)
{
    ESP_LOGI(TAG, "Firmware: %s", build_info_get_string());
    ESP_LOGI(TAG, "ESP-IDF: %d.%d", ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR);

    esp_err_t self_test_ret = self_test_run();
    if (self_test_ret != ESP_OK) {
        ESP_LOGW(TAG, "Self-test returned %s, continuing in degraded mode",
                 esp_err_to_name(self_test_ret));
    }

    /* 看门狗初始化（监视所有长运行任务） */
    ESP_ERROR_CHECK(watchdog_init());

    nvs_flash_init();
    ESP_LOGI(TAG, "NVS initialized");

    commissioning_mgr_init();
    ESP_LOGI(TAG, "Commissioning credentials initialized");

    /* 把自定义 PIN/Discriminator/spake2p 参数同步到 CHIP 协议栈（必须在 start 之前） */
    commissioning_sync_to_stack();
}

/* ==================== 第 1 层：外设 ==================== */

extern "C" void app_init_hardware(void)
{
    /* H2 EN/BOOT：平时拉高，确保协调器处于运行态（OTA 时再由 serial-flasher 改脚） */
    rcp_coprocessor_hold_running();

    button_init(BUTTON_GPIO);
    status_led_init();
    ESP_LOGI(TAG, "Peripherals initialized");

    /* External RTC RX8025T (I2C). Failure is non-fatal: only affects
     * time keeping across power loss; wall-clock is calibrated after WiFi. */
    if (rtc_rx8025t_init(RTC_I2C_SCL_GPIO, RTC_I2C_SDA_GPIO, RTC_I2C_SPEED_HZ) == ESP_OK) {
        ESP_LOGI(TAG, "RX8025T RTC ready");
        rtc_time_diag_start();
    } else {
        ESP_LOGW(TAG, "RX8025T RTC init failed (external RTC absent?)");
    }
}

/* ==================== 第 2 层：Matter 协议栈 ==================== */

extern "C" uint16_t app_init_matter_stack(void)
{
    device_info_init();

    /* 在 esp_matter::start() 前注册自定义 Device Instance Info Provider，
     * 把 Basic Information 的制造商改为 XiaoNian */
    device_instance_provider_register();

    node::config_t node_config;
    node_t *node = node::create(&node_config, bridge_manager_attr_update_cb, NULL);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    aggregator::config_t aggregator_config;
    endpoint_t *aggregator = endpoint::aggregator::create(node, &aggregator_config,
                                                          ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(aggregator != nullptr, ESP_LOGE(TAG, "Failed to create aggregator endpoint"));

    uint16_t aggregator_endpoint_id = endpoint::get_id(aggregator);

    /* 在 start() 之前挂上可续传的 image processor，Requestor 在 DNS-SD 起来时才会真正启动。 */
    esp_err_t err = matter_ota_init();
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to init Matter OTA: %s", esp_err_to_name(err)));

    err = esp_matter::start(bridge_manager_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));
    matter_ota_persist_boot_version();

    err = app_bridge_initialize(node, bridge_manager_create_devices_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to resume bridged endpoints: %d", err));

    /* 重启后若已配网过，标记 Matter 已配网（fabric）。常亮还需检测到 HA 活跃会话。 */
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
        app_status_notify_ready();
    }

    /* 启动就绪确认轮询（检测 HA 活跃会话 → 常亮） */
    status_led_start_ready_poll();

    /* CHIP shell console 与 serial_cmd 任务共享 UART0 会导致 spinlock 冲突，已禁用。
     * 改由 provisioning/serial_cmd.cpp 中的独立任务处理串口命令。 */
#if 0
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
#endif

    return aggregator_endpoint_id;
}

/* ==================== 第 3 层：运行期服务 ==================== */

extern "C" void app_init_runtime_services(uint16_t aggregator_endpoint_id)
{
    bridge_manager_init(aggregator_endpoint_id);

    /* WiFi 管理层初始化（处理断线重连） */
    wifi_manager_init();

    /* 固件 OTA 升级 */
    ota_manager_init();
    ota_manager_start_periodic_check();

    /* Zigbee 子设备 OTA 暂存分区（HTTP 在拿到 WiFi IP 后启动） */
    esp_err_t err = zb_device_ota_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "zb_device_ota_init: %s (child OTA unavailable)", esp_err_to_name(err));
    }
}

/* ==================== 第 4 层：Zigbee 协议栈 ==================== */

extern "C" void app_init_zigbee_stack(void)
{
    zdp_discovery_init();

    /* 注册 Steering ↔ LED 联动回调（并入状态机：入网激活时 500ms 闪烁） */
    zboss_manager_register_steering_callback([](bool active) {
        status_led_set_steering(active);
    });
    ESP_LOGI(TAG, "Steering LED callback registered");

    /* Zigbee 协调器网络组建成功 → 标记 Zigbee 就绪（与已配网共同决定常亮） */
    zigbee_net_register_ready_cb([]() {
        status_led_set_zigbee_ready();
    });
    ESP_LOGI(TAG, "Zigbee network-ready callback registered");

    zboss_manager_start();
}

/* ==================== 第 5 层：串口命令与按键 ==================== */

extern "C" void app_init_console(void)
{
    /* 串口命令监听任务（factoryreset 等） */
    serial_cmd_start();

    /* 按键事件路由：短按开窗，长按重启 */
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
}

/* ==================== 收尾 ==================== */

extern "C" void app_init_finish(void)
{
    ESP_LOGI(TAG, "=== System initialized ===");
    ESP_LOGI(TAG, "Heap: free=%u internal=%u largest_int=%u psram=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* 主任务即将退出，从看门狗中移除 */
    watchdog_remove_self();
}

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
#include <esp_random.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_console.h>

#include <platform/ESP32/ESP32Config.h>

#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Base64.h>

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

    /* 将自定义 PIN/Discriminator 同步到 CHIP 协议栈
     * 必须在 esp_matter::start() 之前调用，同时必须重新生成 spake2p verifier，
     * 否则 PIN 变了但 verifier 没变，PASE 握手会失败（CHIP_ERROR_INVALID_PASE_PARAMETER） */
    {
        uint32_t pin = commissioning_mgr_get_pin();
        uint16_t disc = commissioning_mgr_get_discriminator();
        CHIP_ERROR err;

        err = chip::DeviceLayer::Internal::ESP32Config::WriteConfigValue(
            chip::DeviceLayer::Internal::ESP32Config::kConfigKey_SetupPinCode, pin);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Failed to write PIN: %" CHIP_ERROR_FORMAT, err.Format());
        }
        err = chip::DeviceLayer::Internal::ESP32Config::WriteConfigValue(
            chip::DeviceLayer::Internal::ESP32Config::kConfigKey_SetupDiscriminator, static_cast<uint32_t>(disc));
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Failed to write discriminator: %" CHIP_ERROR_FORMAT, err.Format());
        }

        /* ---- 读取迭代计数（不存在则用默认值） ---- */
        uint32_t iterCount = CHIP_DEVICE_CONFIG_USE_TEST_SPAKE2P_ITERATION_COUNT;
        chip::DeviceLayer::Internal::ESP32Config::ReadConfigValue(
            chip::DeviceLayer::Internal::ESP32Config::kConfigKey_Spake2pIterationCount, iterCount);
        chip::DeviceLayer::Internal::ESP32Config::WriteConfigValue(
            chip::DeviceLayer::Internal::ESP32Config::kConfigKey_Spake2pIterationCount, iterCount);

        /* ---- 读取 salt（不存在则生成随机 salt） ---- */
        constexpr size_t kMaxSaltLen = chip::Crypto::kSpake2p_Max_PBKDF_Salt_Length;
        constexpr size_t kSaltB64Len = BASE64_ENCODED_LEN(kMaxSaltLen) + 1;
        static uint8_t salt[kMaxSaltLen];
        static char saltB64[kSaltB64Len];
        size_t saltLen = kMaxSaltLen;
        size_t saltB64ReadLen = 0;

        err = chip::DeviceLayer::Internal::ESP32Config::ReadConfigValueStr(
            chip::DeviceLayer::Internal::ESP32Config::kConfigKey_Spake2pSalt, saltB64, sizeof(saltB64), saltB64ReadLen);
        if (err == CHIP_NO_ERROR && saltB64ReadLen > 0) {
            saltLen = chip::Base64Decode32(saltB64, static_cast<uint32_t>(saltB64ReadLen), salt);
            ESP_LOGI(TAG, "Using existing spake2p salt (%u bytes)", (unsigned)saltLen);
        } else {
            esp_fill_random(salt, kMaxSaltLen);
            saltLen = kMaxSaltLen;
            chip::Base64Encode32(salt, static_cast<uint32_t>(saltLen), saltB64);
            chip::DeviceLayer::Internal::ESP32Config::WriteConfigValueStr(
                chip::DeviceLayer::Internal::ESP32Config::kConfigKey_Spake2pSalt, saltB64, strlen(saltB64));
            ESP_LOGI(TAG, "Generated new spake2p salt (%u bytes)", (unsigned)saltLen);
        }

        /* ---- 用新 PIN 重新生成 spake2p verifier ---- */
        static chip::Crypto::Spake2pVerifier verifier;
        err = verifier.Generate(iterCount, chip::ByteSpan(salt, saltLen), pin);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "Failed to generate verifier: %" CHIP_ERROR_FORMAT, err.Format());
        } else {
            static chip::Crypto::Spake2pVerifierSerialized serializedVerifier;
            chip::MutableByteSpan verifierSpan(serializedVerifier);
            err = verifier.Serialize(verifierSpan);
            if (err != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "Failed to serialize verifier: %" CHIP_ERROR_FORMAT, err.Format());
            } else {
                static char verifierB64[BASE64_ENCODED_LEN(sizeof(serializedVerifier)) + 1];
                chip::Base64Encode32(serializedVerifier, static_cast<uint32_t>(verifierSpan.size()), verifierB64);
                chip::DeviceLayer::Internal::ESP32Config::WriteConfigValueStr(
                    chip::DeviceLayer::Internal::ESP32Config::kConfigKey_Spake2pVerifier, verifierB64, strlen(verifierB64));
                ESP_LOGI(TAG, "Regenerated spake2p verifier for new PIN");
            }
        }

        ESP_LOGI(TAG, "Synced commissioning data to Matter stack: PIN=%08" PRIu32 ", disc=%u",
                 pin, disc);
    }

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

/* CHIP shell console 与 serial_cmd 任务共享 UART0 会导致 spinlock 冲突，已禁用。
     * 改用下面 app_main 中独立的 serial_cmd 任务处理串口命令。 */
#if 0
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

    /* ---- 串口命令监听任务（factoryreset 等） ---- */
    xTaskCreate([](void *) {
        char line[128];
        while (true) {
            if (fgets(line, sizeof(line), stdin)) {
                char *end = line + strlen(line);
                while (end > line && (*(end - 1) == '\n' || *(end - 1) == '\r')) {
                    *(--end) = '\0';
                }
                if (strcmp(line, "factoryreset") == 0) {
                    ESP_LOGI(TAG, "Factory reset triggered via serial...");
                    esp_matter::factory_reset();
                } else if (strcmp(line, "help") == 0) {
                    ESP_LOGI(TAG, "Commands: factoryreset, help");
                } else if (strlen(line) > 0) {
                    ESP_LOGI(TAG, "Unknown command: '%s' (try 'help')", line);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }, "serial_cmd", 4096, NULL, 2, NULL);

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
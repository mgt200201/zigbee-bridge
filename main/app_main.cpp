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
#include <esp_timer.h>

#include <platform/ESP32/ESP32Config.h>

#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/Base64.h>

/* 自定义 Device Instance Info Provider，用于把 Basic Information 集群的制造商改为 "XiaoNian" */
#include <esp_matter_providers.h>
#include <platform/internal/GenericConfigurationManagerImpl.h>
#include <platform/internal/GenericDeviceInstanceInfoProvider.h>
#include <platform/internal/GenericDeviceInstanceInfoProvider.ipp>

#include <common_macros.h>
#include <app_bridged_device.h>

#include <zboss_manager.h>
#include <zdp_discovery.h>
#include <bridge_manager.h>
#include <device_info.h>
#include <wifi_manager.h>
#include <ota_manager.h>
#include <matter_ota.h>
#include <rcp_coprocessor.h>
#include <commissioning_mgr.h>
#include <button.h>
#include <led.h>
#include <app/server/Server.h>
#include <transport/Session.h>
#include <transport/SecureSession.h>
#include <matter/app_status.h>
#include "build_info.h"
#include "self_test.h"
#include "watchdog.h"
#include "board_config.h"
#include <driver/gpio.h>

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::endpoint;

/* ---------------- 系统状态 LED ----------------
 * 用状态灯（复用 GPIO10）表示运行状态。就绪(常亮)采用"确认就绪"机制：
 *   周期轮询网关的活跃 Matter 安全会话，一旦检测到有控制器（HA）连上来，
 *   且 已配网(fabric) && Zigbee 网络组建成功 两个基础条件也满足，
 *   就立即常亮——这是真实确认，无需猜计时。
 *   活跃的 CASE 安全会话本身就是"网关已就绪、此刻即可被控制"的最强证据，
 *   因此不再依赖 kOperationalNetworkStarted 等可能不在重启/已配网路径触发的信号。
 *   - 未就绪（刚上电 / 条件未齐 / 尚无 HA 会话）：100ms 亮 / 400ms 灭
 *   - 开放入网（Zigbee steering 激活）：500ms 对称闪烁（优先级最高）
 *   - 就绪（已配网 && Zigbee 就绪 && 检测到活跃会话）：常亮
 */
#define STATUS_READY_POLL_MS     2000UL

static volatile bool s_steering_active     = false; /* Zigbee 入网窗口是否开启 */
static volatile bool s_gateway_ready       = false; /* Matter 是否已配网(fabric) */
static volatile bool s_zigbee_ready        = false; /* Zigbee 协调器网络是否组建成功 */
static volatile bool s_confirmed_ready     = false; /* 已确认就绪（检测到活跃会话） */
static bool s_poll_timer_started = false;
static esp_timer_handle_t s_ready_poll_timer = NULL;

/* 两颗独立 LED：
 *  - s_status_led   : 状态灯（启动 100/400 闪烁，就绪常亮）
 *  - s_steering_led : 入网闪烁灯（开放入网时 500ms 间隔闪烁） */
static led_handle_t s_status_led   = NULL;
static led_handle_t s_steering_led = NULL;

static void status_led_refresh(void);

/** 统计当前活跃的 Matter 安全会话数（HA 重连会自动建立 CASE 会话/订阅） */
static int active_secure_session_count(void)
{
    int count = 0;
    auto &mgr = chip::Server::GetInstance().GetSecureSessionManager();
    mgr.ForEachSessionHandle(&count, [](void *ctx, chip::SessionHandle &handle) -> bool {
        if (handle->IsActiveSession()) {
            ++(*(static_cast<int *>(ctx)));
        }
        return true; /* 继续遍历 */
    });
    return count;
}

/** 就绪确认轮询：检测到活跃会话且基础条件齐 → 常亮 */
static void ready_poll_cb(void *arg)
{
    (void)arg;
    if (!s_confirmed_ready && !s_steering_active &&
        s_gateway_ready && s_zigbee_ready &&
        active_secure_session_count() > 0) {
        s_confirmed_ready = true;
        ESP_LOGI(TAG, "Ready confirmed: active Matter session detected");
        status_led_refresh();
    }
}

/** 根据当前状态刷新两颗 LED。
 *  - 入网闪烁灯：开放入网时 500ms 闪烁，否则熄灭
 *  - 状态灯：就绪常亮，未就绪 100ms 亮/400ms 灭 */
static void status_led_refresh(void)
{
    if (s_steering_led) {
        if (s_steering_active) {
            led_blink_start(s_steering_led, STEERING_BLINK_MS);
        } else {
            led_blink_stop(s_steering_led);
        }
    }

    if (s_status_led) {
        if (s_confirmed_ready) {
            led_blink_stop(s_status_led);
            led_on(s_status_led);                       /* 就绪常亮 */
        } else {
            led_blink_start_pattern(s_status_led, LED_BOOT_ON_MS, LED_BOOT_OFF_MS);
        }
    }
}

/** 由 bridge_manager / app_main 调用：标记已配网(fabric)并刷新 LED */
extern "C" void app_status_notify_ready(void)
{
    s_gateway_ready = true;
    status_led_refresh();
}

/** 启动就绪确认轮询（在 esp_matter::start() 之后调用） */
static void status_ready_poll_start(void)
{
    if (s_poll_timer_started) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = &ready_poll_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "status_poll",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_ready_poll_timer) == ESP_OK) {
        esp_timer_start_periodic(s_ready_poll_timer, STATUS_READY_POLL_MS * 1000);
        s_poll_timer_started = true;
    }
}

/* ---------------- 自定义 Device Instance Info Provider ----------------
 * Matter Basic Information 集群的 VendorName(制造商) 读取走
 * DeviceInstanceInfoProvider::GetVendorName()。这里覆盖它，让所有通过本网关
 * 暴露到 HA 的设备制造商都显示为 "XiaoNian"。其余设备信息沿用默认实现。
 */
namespace {

using XiaoNianDeviceInfoBase =
    chip::DeviceLayer::Internal::GenericDeviceInstanceInfoProvider<chip::DeviceLayer::Internal::ESP32Config>;

using XiaoNianConfigMgrBase =
    chip::DeviceLayer::Internal::GenericConfigurationManagerImpl<chip::DeviceLayer::Internal::ESP32Config>;

class XiaoNianDeviceInstanceInfoProvider : public XiaoNianDeviceInfoBase
{
public:
    XiaoNianDeviceInstanceInfoProvider()
        : XiaoNianDeviceInfoBase(static_cast<XiaoNianConfigMgrBase &>(chip::DeviceLayer::ConfigurationMgr())) {}

    CHIP_ERROR GetVendorName(char *buf, size_t bufSize) override
    {
        constexpr char kXiaoNian[] = "XiaoNian";
        VerifyOrReturnError(bufSize >= sizeof(kXiaoNian), CHIP_ERROR_BUFFER_TOO_SMALL);
        memcpy(buf, kXiaoNian, sizeof(kXiaoNian));
        return CHIP_NO_ERROR;
    }
};

void register_xiaonian_manufacturer(void)
{
    static XiaoNianDeviceInstanceInfoProvider s_provider;
    esp_matter::set_custom_device_instance_info_provider(&s_provider);
    ESP_LOGI(TAG, "Manufacturer set to XiaoNian");
}

static void gateway_erase_bridge_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open_from_partition("nvs", "bridge", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "No Matter bridge NVS to erase");
        return;
    }
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Erased Matter bridged-endpoint NVS");
}

} /* namespace */

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

    /* H2 EN/BOOT：平时拉高，确保协调器处于运行态（OTA 时再由 serial-flasher 改脚） */
    rcp_coprocessor_hold_running();

    button_init(BUTTON_GPIO);
    s_status_led   = led_create(STATUS_LED_GPIO,   STATUS_LED_POLARITY);
    s_steering_led = led_create(STEERING_LED_GPIO, STEERING_LED_POLARITY);
    status_led_refresh();         /* 刚上电：状态灯 100ms 亮/400ms 灭 */
    ESP_LOGI(TAG, "Peripherals initialized");

    /* ---- 第 2 层：Matter 协议栈初始化 ---- */
    device_info_init();

    /* 在 esp_matter::start() 前注册自定义 Device Instance Info Provider，把制造商改为 XiaoNian */
    register_xiaonian_manufacturer();

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
    status_ready_poll_start();

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

    /* 注册 Steering ↔ LED 联动回调（并入状态机：入网激活时 500ms 闪烁） */
    zboss_manager_register_steering_callback([](bool active) {
        s_steering_active = active;
        status_led_refresh();
    });
    ESP_LOGI(TAG, "Steering LED callback registered");

    /* Zigbee 协调器网络组建成功 → 标记 Zigbee 就绪（与已配网共同决定常亮） */
    zigbee_net_register_ready_cb([]() {
        s_zigbee_ready = true;
        status_led_refresh();
    });
    ESP_LOGI(TAG, "Zigbee network-ready c llback registered");

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
                    gateway_erase_bridge_nvs();
                    device_info_clear_all();
                    esp_matter::factory_reset();
                } else if (strcmp(line, "entest") == 0 || strcmp(line, "bootest") == 0) {
                    const gpio_num_t pin = (strcmp(line, "bootest") == 0) ? RCP_BOOT_GPIO : RCP_EN_GPIO;
                    const char *name = (pin == RCP_BOOT_GPIO) ? "BOOT" : "EN";
                    gpio_config_t io = {};
                    io.mode = GPIO_MODE_OUTPUT;
                    io.pin_bit_mask = 1ULL << pin;
                    gpio_config(&io);
                    ESP_LOGI(TAG, "%s: GPIO%d 2s LOW / 2s HIGH x8, meter on this pin vs GND",
                             name, (int)pin);
                    for (int i = 0; i < 8; i++) {
                        gpio_set_level(pin, 0);
                        ESP_LOGI(TAG, "%s [%d/8] GPIO%d = 0 (LOW)", name, i + 1, (int)pin);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        gpio_set_level(pin, 1);
                        ESP_LOGI(TAG, "%s [%d/8] GPIO%d = 1 (HIGH)", name, i + 1, (int)pin);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                    gpio_set_level(pin, 1);
                    ESP_LOGI(TAG, "%s done, GPIO%d left HIGH", name, (int)pin);
                } else if (strcmp(line, "dltest") == 0) {
                    /* BOOT 拉低后脉冲 EN，保持 15s。H2 若进 ROM，Zigbee 会一直断。 */
                    gpio_config_t io = {};
                    io.mode = GPIO_MODE_OUTPUT;
                    io.pin_bit_mask = (1ULL << RCP_EN_GPIO) | (1ULL << RCP_BOOT_GPIO);
                    gpio_config(&io);
                    gpio_set_level(RCP_BOOT_GPIO, 0);
                    vTaskDelay(pdMS_TO_TICKS(10));
                    gpio_set_level(RCP_EN_GPIO, 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    gpio_set_level(RCP_EN_GPIO, 1);
                    ESP_LOGI(TAG, "dltest: BOOT=GPIO%d LOW, EN pulsed. Hold 15s",
                             (int)RCP_BOOT_GPIO);
                    vTaskDelay(pdMS_TO_TICKS(15000));
                    gpio_set_level(RCP_BOOT_GPIO, 1);
                    gpio_set_level(RCP_EN_GPIO, 0);
                    vTaskDelay(pdMS_TO_TICKS(20));
                    gpio_set_level(RCP_EN_GPIO, 1);
                    ESP_LOGI(TAG, "dltest done, H2 released to run");
                } else if (strcmp(line, "iostest") == 0) {
                    const gpio_num_t pins[] = {
                        GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_3, GPIO_NUM_46,
                    };
                    const char *names[] = {
                        "IO8", "IO9", "IO3(BOOT->H2 GPIO9)", "IO46(EN)",
                    };
                    ESP_LOGI(TAG, "iostest: S3 levels  float / pullup / pulldown");
                    for (size_t i = 0; i < 4; i++) {
                        gpio_reset_pin(pins[i]);
                        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
                        gpio_set_pull_mode(pins[i], GPIO_FLOATING);
                        vTaskDelay(pdMS_TO_TICKS(5));
                        const int f = gpio_get_level(pins[i]);
                        gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
                        vTaskDelay(pdMS_TO_TICKS(5));
                        const int u = gpio_get_level(pins[i]);
                        gpio_set_pull_mode(pins[i], GPIO_PULLDOWN_ONLY);
                        vTaskDelay(pdMS_TO_TICKS(5));
                        const int d = gpio_get_level(pins[i]);
                        gpio_set_pull_mode(pins[i], GPIO_FLOATING);
                        const char *hint = "mixed";
                        if (f == 1 && u == 1 && d == 1) {
                            hint = "strong HIGH";
                        } else if (f == 0 && u == 0 && d == 0) {
                            hint = "strong LOW";
                        } else if (u == 1 && d == 0) {
                            hint = "floating / weak";
                        }
                        ESP_LOGI(TAG, "  %s GPIO%d: float=%d pu=%d pd=%d (%s)",
                                 names[i], (int)pins[i], f, u, d, hint);
                    }
                    gpio_config_t restore = {};
                    restore.mode = GPIO_MODE_OUTPUT;
                    restore.pin_bit_mask = (1ULL << RCP_EN_GPIO) | (1ULL << RCP_BOOT_GPIO);
                    gpio_config(&restore);
                    gpio_set_level(RCP_BOOT_GPIO, 1);
                    gpio_set_level(RCP_EN_GPIO, 1);
                    ESP_LOGI(TAG, "iostest done (S3 pins; H2 GPIO8/9 only if wired here)");
                } else if (strcmp(line, "synctest") == 0) {
                    ESP_LOGI(TAG, "synctest: connecting H2 ROM (Zigbee UART stolen, will reboot)");
                    esp_err_t err = rcp_coprocessor_sync_test();
                    ESP_LOGI(TAG, "synctest result: %s", esp_err_to_name(err));
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                } else if (strcmp(line, "help") == 0) {
                    ESP_LOGI(TAG, "Commands: factoryreset, entest, bootest, dltest, iostest, synctest, help");
                } else if (strlen(line) > 0) {
                    ESP_LOGI(TAG, "Unknown command: '%s' (try 'help')", line);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }, "serial_cmd", 8192, NULL, 2, NULL);

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
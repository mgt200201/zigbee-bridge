/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zboss_manager.cpp
 *
 * @brief Zigbee 网络管理层
 *
 * 负责 Zigbee 协议栈的初始化、网络组建、入网许可控制、
 * 已知设备管理、桥接端点映射等核心功能。
 *
 * 本层不感知 Matter 协议，所有跨层调用由 app_main.cpp 编排。
 */

#include "zboss_manager.h"
#include "zdp_discovery.h"
#include <string.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_zigbee_core.h>
#include <esp_zigbee_secur.h>
#include <test/esp_zigbee_test_utils.h>
#include <ha/esp_zigbee_ha_standard.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "zboss_manager";

/** 入网窗口持续时间（180秒），超时后自动关闭 */
#define STEERING_DURATION_SEC   180

/* ==================== 全局状态变量 ==================== */

/** Zigbee 网络是否已组建完成 */
static bool s_network_formed = false;
/** 入网窗口是否处于开放状态 */
static bool s_steering_active = false;
/** 入网窗口开始时间（毫秒） */
static int s_steering_start_time = 0;

/** Steering 状态回调（供 LED 等外部反馈） */
static steering_state_callback_t s_steering_cb = NULL;
/** Steering 超时检测定时器句柄 */
static esp_timer_handle_t s_steering_timer = NULL;

/* ==================== 已知设备列表 ==================== */
static known_device_t s_known_devices[MAX_KNOWN_DEVICES] = {0};

/* ==================== 桥接端点映射 ==================== */

/** 最大桥接端点数量 */
#define MAX_BRIDGED_ENDPOINTS 32

/**
 * @brief 桥接端点映射结构
 *
 * 维护 Zigbee 端点到短地址的映射关系，用于将收到的 ZCL 报告
 * 路由到正确的桥接设备。
 */
typedef struct {
    bool in_use;
    uint16_t short_addr;
    uint8_t zigbee_endpoint;
} bridged_ep_map_t;

static bridged_ep_map_t s_bridged_eps[MAX_BRIDGED_ENDPOINTS] = {0};

/* ==================== 内部函数声明 ==================== */
static void add_or_update_known_device(const esp_zb_ieee_addr_t ieee, uint16_t short_addr);
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask);
static void zboss_task(void *pvParameters);

/* ==================== 已知设备管理 ==================== */

known_device_t *zboss_manager_find_by_ieee(const esp_zb_ieee_addr_t ieee)
{
    for (int i = 0; i < MAX_KNOWN_DEVICES; i++) {
        if (s_known_devices[i].in_use && memcmp(s_known_devices[i].ieee_addr, ieee, sizeof(esp_zb_ieee_addr_t)) == 0) {
            return &s_known_devices[i];
        }
    }
    return NULL;
}

known_device_t *zboss_manager_find_by_short(uint16_t short_addr)
{
    for (int i = 0; i < MAX_KNOWN_DEVICES; i++) {
        if (s_known_devices[i].in_use && s_known_devices[i].short_addr == short_addr) {
            return &s_known_devices[i];
        }
    }
    return NULL;
}

known_device_t *zboss_manager_get_known_devices(void)
{
    return s_known_devices;
}

static void add_or_update_known_device(const esp_zb_ieee_addr_t ieee, uint16_t short_addr)
{
    known_device_t *dev = zboss_manager_find_by_ieee(ieee);
    if (dev) {
        if (dev->short_addr != short_addr) {
            ESP_LOGI(TAG, "Known device IEEE updated short addr 0x%04hx -> 0x%04hx", dev->short_addr, short_addr);
            /* 更新桥接端点映射表中的短地址 */
            for (int i = 0; i < MAX_BRIDGED_ENDPOINTS; i++) {
                if (s_bridged_eps[i].in_use && s_bridged_eps[i].short_addr == dev->short_addr) {
                    s_bridged_eps[i].short_addr = short_addr;
                }
            }
            dev->short_addr = short_addr;
        }
        return;
    }
    dev = zboss_manager_find_by_short(short_addr);
    if (dev) {
        memcpy(dev->ieee_addr, ieee, sizeof(esp_zb_ieee_addr_t));
        return;
    }
    for (int i = 0; i < MAX_KNOWN_DEVICES; i++) {
        if (!s_known_devices[i].in_use) {
            s_known_devices[i].in_use = true;
            memcpy(s_known_devices[i].ieee_addr, ieee, sizeof(esp_zb_ieee_addr_t));
            s_known_devices[i].short_addr = short_addr;
            ESP_LOGI(TAG, "Added known device 0x%04hx to list", short_addr);
            return;
        }
    }
    ESP_LOGW(TAG, "Known device list full");
}

/* ==================== 桥接端点映射 ==================== */

void zboss_manager_register_bridged_device(uint16_t short_addr, uint8_t zigbee_endpoint)
{
    known_device_t *dev = zboss_manager_find_by_short(short_addr);
    if (!dev) {
        /* 若已知设备列表中也找不到，仅记录日志，不自动添加 */
        ESP_LOGW(TAG, "Device 0x%04hx not in known list, adding anyway", short_addr);
    }
    for (int i = 0; i < MAX_BRIDGED_ENDPOINTS; i++) {
        if (!s_bridged_eps[i].in_use) {
            s_bridged_eps[i].in_use = true;
            s_bridged_eps[i].short_addr = short_addr;
            s_bridged_eps[i].zigbee_endpoint = zigbee_endpoint;
            ESP_LOGI(TAG, "Registered bridged zigbee ep: short_addr=0x%04hx, ep=%d", short_addr, zigbee_endpoint);
            return;
        }
    }
    ESP_LOGW(TAG, "Bridged endpoint map full");
}

uint16_t zboss_manager_find_short_addr_by_ep(uint8_t zigbee_endpoint)
{
    for (int i = 0; i < MAX_BRIDGED_ENDPOINTS; i++) {
        if (s_bridged_eps[i].in_use && s_bridged_eps[i].zigbee_endpoint == zigbee_endpoint) {
            return s_bridged_eps[i].short_addr;
        }
    }
    return 0xFFFF;
}

uint8_t zboss_manager_find_ep_by_short(uint16_t short_addr)
{
    for (int i = 0; i < MAX_BRIDGED_ENDPOINTS; i++) {
        if (s_bridged_eps[i].in_use && s_bridged_eps[i].short_addr == short_addr) {
            return s_bridged_eps[i].zigbee_endpoint;
        }
    }
    return 0xFF;
}

/* ==================== 网络状态管理 ==================== */

bool zboss_manager_is_network_formed(void)
{
    return s_network_formed;
}

bool zboss_manager_is_steering_active(void)
{
    return s_steering_active;
}

void zboss_manager_start_steering(void)
{
    if (!s_network_formed) {
        ESP_LOGW(TAG, "Network not formed, cannot start steering");
        return;
    }
    s_steering_active = true;
    s_steering_start_time = (int)(esp_timer_get_time() / 1000ULL);
    ESP_LOGI(TAG, "Steering started, duration=%d sec", STEERING_DURATION_SEC);
    /* 通知外部回调（如 LED 闪烁） */
    if (s_steering_cb) {
        s_steering_cb(true);
    }
    /* 通过 ZBOSS scheduler 在 ZBOSS 任务上下文中执行 BDB steering */
    esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                           ESP_ZB_BDB_MODE_NETWORK_STEERING, 0);
}

void zboss_manager_stop_steering(void)
{
    if (!s_steering_active) {
        return;
    }
    s_steering_active = false;
    s_steering_start_time = 0;
    ESP_LOGI(TAG, "Steering stopped");
    /* 通知外部回调（如 LED 停止闪烁） */
    if (s_steering_cb) {
        s_steering_cb(false);
    }
}

/**
 * @brief 检查 steering 是否超时（供外部定时器调用）
 * @return true 已超时需要关闭
 */
bool zboss_manager_steering_is_expired(void)
{
    if (!s_steering_active) {
        return false;
    }
    int now = (int)(esp_timer_get_time() / 1000ULL);
    return (now - s_steering_start_time) >= (STEERING_DURATION_SEC * 1000);
}

/* ==================== Steering 回调注册 ==================== */

void zboss_manager_register_steering_callback(steering_state_callback_t cb)
{
    s_steering_cb = cb;
}

/* ==================== Steering 超时定时器 ==================== */

static void steering_timer_cb(void *arg)
{
    (void)arg;
    if (zboss_manager_steering_is_expired()) {
        ESP_LOGI(TAG, "Steering timeout expired, closing window");
        zboss_manager_stop_steering();
    }
}

/* ==================== 按键短按处理 ==================== */

void zboss_manager_handle_short_press(void)
{
    if (!s_network_formed) {
        ESP_LOGI(TAG, "Network not formed yet, ignoring button press");
        return;
    }
    if (zboss_manager_is_steering_active()) {
        zboss_manager_stop_steering();
    } else {
        zboss_manager_start_steering();
    }
}

/* ==================== ZBOSS 回调 ==================== */

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

/* ==================== ZBOSS 信号处理 ==================== */

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = static_cast<esp_zb_app_signal_type_t>(*p_sg_p);
    esp_zb_zdo_signal_device_annce_params_t *dev_annce_params = NULL;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialized");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Start network formation");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        } else {
            ESP_LOGE(TAG, "Failed to initialize Zigbee stack (status: %d)", err_status);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t ieee_address;
            esp_zb_get_long_address(ieee_address);
            ESP_LOGI(TAG, "Formed network successfully");
            ESP_LOGI(TAG, "PAN ID: 0x%04hx", esp_zb_get_pan_id());

            uint8_t network_key[16] = {0};
            if (esp_zb_secur_primary_network_key_get(network_key) == ESP_OK) {
                ESP_LOGI(TAG, "Network Key (for Wireshark decryption):");
                ESP_LOG_BUFFER_HEX(TAG, network_key, 16);
            }

            s_network_formed = true;
        } else {
            ESP_LOGI(TAG, "Restart network formation (status: %d)", err_status);
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "BDB steering success");
        } else {
            ESP_LOGW(TAG, "BDB steering not successful (status: %d), retry if window still open", err_status);
            if (s_steering_active) {
                esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
            }
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
        dev_annce_params = (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        ESP_LOGI(TAG, "New device commissioned or rejoined (short: 0x%04hx)", dev_annce_params->device_short_addr);
        add_or_update_known_device(dev_annce_params->ieee_addr, dev_annce_params->device_short_addr);
        /* 通知设备发现模块处理新设备 */
        zdp_discovery_handle_device_annce(dev_annce_params->device_short_addr,
                                          dev_annce_params->ieee_addr,
                                          s_steering_active);
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        esp_zb_zdo_signal_leave_indication_params_t *leave_params =
            (esp_zb_zdo_signal_leave_indication_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        ESP_LOGI(TAG, "Device leaving indication (short: 0x%04hx, rejoin: %d)",
                 leave_params->short_addr, leave_params->rejoin);
        break;
    }

    default:
        ESP_LOGI(TAG, "Zigbee signal: type=%d, status=%d", sig_type, err_status);
        break;
    }
}

/* ==================== ZBOSS 主任务 ==================== */

static void zboss_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();
    zb_nwk_cfg.nwk_cfg.zczr_cfg.max_children = 16;
    esp_zb_secur_set_unsecure_tc_rejoin_enabled(true);
    esp_zb_secur_tcpol_set_allow_tc_rejoins(1);
    esp_zb_init(&zb_nwk_cfg);

    /* 终端设备老化超时设为最大值 */
    esp_zb_nwk_set_ed_timeout(ESP_ZB_ED_AGING_TIMEOUT_16384MIN);
    /* 拒绝子设备 Leave 请求 */
    esp_zb_nwk_set_leave_req_allowed(false);

    /* 创建协调器端点（含温度/湿度客户端集群，接收子设备报告） */
    esp_zb_on_off_switch_cfg_t switch_cfg = ESP_ZB_DEFAULT_ON_OFF_SWITCH_CONFIG();
    esp_zb_cluster_list_t *cluster_list = esp_zb_on_off_switch_clusters_create(&switch_cfg);

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = { .measured_value = 0, .min_value = -2000, .max_value = 2000 };
    esp_zb_attribute_list_t *temp_cluster = esp_zb_temperature_meas_cluster_create(&temp_cfg);
    esp_zb_cluster_list_add_temperature_meas_cluster(cluster_list, temp_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_humidity_meas_cluster_cfg_t humidity_cfg = { .measured_value = 0, .min_value = 0, .max_value = 10000 };
    esp_zb_attribute_list_t *humidity_cluster = esp_zb_humidity_meas_cluster_create(&humidity_cfg);
    esp_zb_cluster_list_add_humidity_meas_cluster(cluster_list, humidity_cluster, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = HA_GW_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID,
        .app_device_version = 1,
    };
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);

    esp_zb_device_register(ep_list);

    /* 注册 ZCL 动作处理器（由 zdp_discovery 提供） */
    esp_zb_core_action_handler_register(zdp_discovery_get_action_handler());

    /* 设置主信道并启动协议栈 */
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    /* 进入 Zigbee 主循环（永不返回） */
    esp_zb_stack_main_loop();
}

/* ==================== 启动入口 ==================== */

void zboss_manager_start(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = {
            .radio_mode = ZB_RADIO_MODE_UART_RCP,
            .radio_uart_config = {
                .port = UART_NUM_1,
                .rx_pin = (gpio_num_t)CONFIG_PIN_TO_RCP_TX,
                .tx_pin = (gpio_num_t)CONFIG_PIN_TO_RCP_RX,
                .uart_config = {
                    .baud_rate = 460800,
                    .data_bits = UART_DATA_8_BITS,
                    .parity = UART_PARITY_DISABLE,
                    .stop_bits = UART_STOP_BITS_1,
                    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                    .rx_flow_ctrl_thresh = 0,
                    .source_clk = UART_SCLK_APB,
                },
            },
        },
        .host_config = {
            .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
        },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    xTaskCreate(zboss_task, "zboss_main", 10240, NULL, 5, NULL);

    /* 创建 Steering 超时检测定时器（每秒检查一次） */
    const esp_timer_create_args_t timer_args = {
        .callback = &steering_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "steering_tmr",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &s_steering_timer) == ESP_OK) {
        esp_timer_start_periodic(s_steering_timer, 1000000);
        ESP_LOGI(TAG, "Steering timeout timer started");
    } else {
        ESP_LOGW(TAG, "Failed to create steering timeout timer");
    }
}
/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app_zboss.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_zigbee_core.h>
#include <esp_zigbee_secur.h>
#include <zdo/esp_zigbee_zdo_command.h>
#include <ha/esp_zigbee_ha_standard.h>
#include <test/esp_zigbee_test_utils.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_matter.h>
#include <zigbee_bridge.h>
#include <app_bridged_device.h>

extern uint16_t aggregator_endpoint_id;

using namespace esp_matter;
using namespace esp_matter::cluster;

/* 按键配置：外接按键到 GND（低电平有效） */
#define BUTTON_GPIO             GPIO_NUM_9
#define LED_GPIO                GPIO_NUM_10
#define SHORT_PRESS_MAX_MS      3000    /* 短按最大 3 秒 */
#define DEBOUNCE_MS             30      /* 按键防抖 30ms */
#define STEERING_DURATION_SEC   180     /* 开放入网 180 秒 */
#define LED_BLINK_INTERVAL_MS   500     /* LED 闪烁间隔 500ms */
#define HA_GW_ENDPOINT          1       /* 网关 Zigbee endpoint，用于接收子设备报告 */

static bool network_formed = false;
static bool steering_active = false;
static int steering_start_time = 0;

static void stop_steering(void)
{
    steering_active = false;
    gpio_set_level(LED_GPIO, 0);
}


#if (!defined(ZB_MACSPLIT_HOST) && defined(ZB_MACSPLIT_DEVICE))
#error "Zigbee host option should be enabled to use this example"
#endif

static const char *TAG = "esp_zboss";

static uint16_t pending_annce_short_addr = 0xFFFF;
static esp_zb_ieee_addr_t pending_annce_ieee = {0};

#define MAX_KNOWN_DEVICES 16
typedef struct {
    bool in_use;
    esp_zb_ieee_addr_t ieee_addr;
    uint16_t short_addr;
} known_device_t;
static known_device_t s_known_devices[MAX_KNOWN_DEVICES] = {0};

static known_device_t *find_known_device_by_ieee(const esp_zb_ieee_addr_t ieee)
{
    for (int i = 0; i < MAX_KNOWN_DEVICES; i++) {
        if (s_known_devices[i].in_use && memcmp(s_known_devices[i].ieee_addr, ieee, sizeof(esp_zb_ieee_addr_t)) == 0) {
            return &s_known_devices[i];
        }
    }
    return NULL;
}

static known_device_t *find_known_device_by_short(uint16_t short_addr)
{
    for (int i = 0; i < MAX_KNOWN_DEVICES; i++) {
        if (s_known_devices[i].in_use && s_known_devices[i].short_addr == short_addr) {
            return &s_known_devices[i];
        }
    }
    return NULL;
}

static void add_or_update_known_device(const esp_zb_ieee_addr_t ieee, uint16_t short_addr)
{
    known_device_t *dev = find_known_device_by_ieee(ieee);
    if (dev) {
        if (dev->short_addr != short_addr) {
            ESP_LOGI(TAG, "Known device IEEE updated short addr 0x%04hx -> 0x%04hx", dev->short_addr, short_addr);
            app_bridged_device_t *old_dev = app_bridge_get_device_by_zigbee_shortaddr(dev->short_addr);
            if (old_dev) {
                ESP_LOGI(TAG, "Remove old bridged endpoint for short addr 0x%04hx", dev->short_addr);
                app_bridge_remove_device(old_dev);
            }
            dev->short_addr = short_addr;
        }
        return;
    }
    dev = find_known_device_by_short(short_addr);
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

#define MAX_BRIDGED_ENDPOINTS 32
typedef struct {
    bool in_use;
    uint16_t short_addr;
    uint8_t zigbee_endpoint;
} bridged_ep_map_t;
static bridged_ep_map_t s_bridged_eps[MAX_BRIDGED_ENDPOINTS] = {0};

static uint16_t find_short_addr_by_zigbee_endpoint(uint8_t zigbee_endpoint)
{
    for (int i = 0; i < MAX_BRIDGED_ENDPOINTS; i++) {
        if (s_bridged_eps[i].in_use && s_bridged_eps[i].zigbee_endpoint == zigbee_endpoint) {
            return s_bridged_eps[i].short_addr;
        }
    }
    return 0xFFFF;
}

extern "C" void app_zboss_register_bridged_device(uint16_t short_addr, uint8_t zigbee_endpoint)
{
    known_device_t *dev = find_known_device_by_short(short_addr);
    if (!dev) {
        for (int i = 0; i < MAX_KNOWN_DEVICES; i++) {
            if (!s_known_devices[i].in_use) {
                s_known_devices[i].in_use = true;
                memcpy(s_known_devices[i].ieee_addr, pending_annce_ieee, sizeof(esp_zb_ieee_addr_t));
                s_known_devices[i].short_addr = short_addr;
                ESP_LOGI(TAG, "Registered bridged device 0x%04hx", short_addr);
            }
        }
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

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

static void find_temp_humidity_sensor_cb(uint8_t param)
{
    (void)param;
    if (pending_annce_short_addr == 0xFFFF) {
        return;
    }
    uint16_t short_addr = pending_annce_short_addr;
    pending_annce_short_addr = 0xFFFF;

    if (app_bridge_get_device_by_zigbee_shortaddr(short_addr)) {
        ESP_LOGI(TAG, "Device 0x%04hx already has a bridged endpoint, skip discovery", short_addr);
        return;
    }

    ESP_LOGI(TAG, "Discovering temp/humidity sensor on 0x%04hx", short_addr);
    esp_zb_zdo_match_desc_req_param_t cmd_req;
    cmd_req.dst_nwk_addr = short_addr;
    cmd_req.addr_of_interest = short_addr;
    cmd_req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    uint16_t cluster_list[] = { ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT };
    cmd_req.num_in_clusters = 1;
    cmd_req.num_out_clusters = 0;
    cmd_req.cluster_list = cluster_list;
    esp_zb_zdo_match_cluster(&cmd_req, zigbee_bridge_find_temp_humidity_sensor_cb, NULL);
}


/**
 * @brief Zigbee stack event handler.
 *
 * @param bufid   Reference to the Zigbee stack buffer used to pass signal.
 */

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    // Read signal description
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
            ESP_LOGI(TAG, "ieee extended address: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx)",
                     ieee_address[7], ieee_address[6], ieee_address[5], ieee_address[4], ieee_address[3],
                     ieee_address[2], ieee_address[1], ieee_address[0], esp_zb_get_pan_id());
            
            /* 获取并打印 Network Key 以便 Wireshark 解密 */
            uint8_t network_key[16] = {0};
            if (esp_zb_secur_primary_network_key_get(network_key) == ESP_OK) {
                ESP_LOGI(TAG, "Network Key (for Wireshark decryption):");
                ESP_LOG_BUFFER_HEX(TAG, network_key, 16);
            }

            network_formed = true;
            ESP_LOGI(TAG, "短按 BOOT 按键可开放入网 %d 秒", STEERING_DURATION_SEC);
        } else {
            ESP_LOGI(TAG, "Restart network formation (status: %d)", err_status);
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "BDB steering 成功，设备已加入或网络已开放");
            /* 协调器上 steering 成功后网络仍开放 180 秒，LED 继续闪烁，由 button_timer 超时关闭 */
        } else {
            ESP_LOGW(TAG, "BDB steering 未成功 (status: %d)，如仍在入网窗口则 1 秒后重试", err_status);
            if (steering_active) {
                esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
            }
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
        dev_annce_params = (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        ESP_LOGI(TAG, "New device commissioned or rejoined (short: 0x%04hx)", dev_annce_params->device_short_addr);
        add_or_update_known_device(dev_annce_params->ieee_addr, dev_annce_params->device_short_addr);
        if (app_bridge_get_device_by_zigbee_shortaddr(dev_annce_params->device_short_addr)) {
            ESP_LOGI(TAG, "Device 0x%04hx already has a bridged endpoint, skip discovery", dev_annce_params->device_short_addr);
            break;
        }
        if (!steering_active && !find_known_device_by_ieee(dev_annce_params->ieee_addr)) {
            ESP_LOGI(TAG, "Steering inactive, ignore new device 0x%04hx", dev_annce_params->device_short_addr);
            break;
        }
        /* 延迟 500ms 再发现，给 end device 留出 rejoin 后的初始化/poll 时间 */
        pending_annce_short_addr = dev_annce_params->device_short_addr;
        memcpy(pending_annce_ieee, dev_annce_params->ieee_addr, sizeof(esp_zb_ieee_addr_t));
        esp_zb_scheduler_alarm((esp_zb_callback_t)find_temp_humidity_sensor_cb, 0, 500);
        break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        esp_zb_zdo_signal_leave_indication_params_t *leave_params =
            (esp_zb_zdo_signal_leave_indication_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        ESP_LOGI(TAG, "Device leaving indication (short: 0x%04hx, rejoin: %d)", leave_params->short_addr,
                 leave_params->rejoin);
        if (leave_params->rejoin == 0) {
            app_bridged_device_t *bridged_device = app_bridge_get_device_by_zigbee_shortaddr(leave_params->short_addr);
            if (bridged_device) {
                ESP_LOGI(TAG, "Removing bridged device for short addr 0x%04hx", leave_params->short_addr);
                app_bridge_remove_device(bridged_device);
            }
        } else {
            ESP_LOGI(TAG, "Device 0x%04hx is expected to rejoin, keep bridged endpoint", leave_params->short_addr);
        }
        break;
    }

    default:
        ESP_LOGI(TAG, "status: %d", err_status);
        break;
    }
}

static void parse_and_log_attribute(uint8_t src_ep, uint8_t dst_ep, uint16_t cluster, const esp_zb_zcl_attribute_t *attribute)
{
    ESP_LOGI(TAG, "ZCL report: src_ep=%d, dst_ep=%d, cluster=0x%04x, attr=0x%04x, size=%d",
             src_ep, dst_ep, cluster, attribute->id, attribute->data.size);

    switch (cluster) {
    case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
        if (attribute->id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && attribute->data.size == 1) {
            uint8_t on_off = *(uint8_t *)attribute->data.value;
            ESP_LOGI(TAG, "  -> On/Off state: %s", on_off ? "ON" : "OFF");
        }
        break;

    case ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT:
        if (attribute->id == ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID && attribute->data.size == 2) {
            int16_t raw = *(int16_t *)attribute->data.value;
            ESP_LOGI(TAG, "  -> Temperature: %.2f C (raw=%d)", raw / 100.0, raw);
        }
        break;

    case 0x0405:
        if (attribute->id == 0x0000 &&
            attribute->data.size == 2) {
            uint16_t raw = *(uint16_t *)attribute->data.value;
            ESP_LOGI(TAG, "  -> Humidity: %.2f %% (raw=%u)", raw / 100.0, raw);
        }
        break;

    default:
        ESP_LOGI(TAG, "  -> Unhandled cluster 0x%04x", cluster);
        break;
    }
}

static esp_err_t zb_set_attr_value_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (!message) {
        return ESP_FAIL;
    }
    if (message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Received ZCL set attr with error status(%d)", message->info.status);
        return ESP_ERR_INVALID_ARG;
    }
    parse_and_log_attribute(message->info.dst_endpoint, message->info.dst_endpoint,
                            message->info.cluster, &message->attribute);
    return ESP_OK;
}

static esp_err_t zb_report_attr_handler(const esp_zb_zcl_report_attr_message_t *message)
{
    if (!message) {
        return ESP_FAIL;
    }
    if (message->status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Received ZCL report with error status(%d)", message->status);
        return ESP_ERR_INVALID_ARG;
    }
    parse_and_log_attribute(message->src_endpoint, message->dst_endpoint,
                            message->cluster, &message->attribute);

    if (message->cluster != ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT &&
        message->cluster != 0x0405) {
        return ESP_OK;
    }

    uint16_t short_addr = find_short_addr_by_zigbee_endpoint(message->src_endpoint);
    if (short_addr == 0xFFFF) {
        ESP_LOGW(TAG, "Zigbee endpoint %d not found in bridged mapping", message->src_endpoint);
        known_device_t *dev = NULL;
        for (int i = 0; i < MAX_KNOWN_DEVICES; i++) {
            if (s_known_devices[i].in_use) {
                dev = &s_known_devices[i];
                break;
            }
        }
        if (dev) {
            short_addr = dev->short_addr;
            ESP_LOGI(TAG, "Found device short_addr=0x%04x from known devices, registering endpoint %d", short_addr, message->src_endpoint);
            app_bridged_device_t *bridged_device = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
            if (!bridged_device) {
                node_t *node = esp_matter::node::get();
                if (node) {
                    bridged_device = app_bridge_create_bridged_device(node, aggregator_endpoint_id,
                                                                     ESP_MATTER_TEMP_HUMIDITY_SENSOR_DEVICE_TYPE_ID,
                                                                     ESP_MATTER_BRIDGED_DEVICE_TYPE_ZIGBEE,
                                                                     app_bridge_zigbee_address(message->src_endpoint, short_addr), NULL);
                }
            }
            if (bridged_device) {
                app_zboss_register_bridged_device(short_addr, message->src_endpoint);
                ESP_LOGI(TAG, "Auto-registered bridged device for short_addr=0x%04x, ep=%d", short_addr, message->src_endpoint);
            }
        } else {
            ESP_LOGW(TAG, "No known devices found");
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "ZCL report for bridging: src_ep=%d, short_addr=0x%04x, cluster=0x%04x",
             message->src_endpoint, short_addr, message->cluster);

    if (message->cluster == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT &&
        message->attribute.id == ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID &&
        message->attribute.data.size == 2) {
        int16_t raw = *(int16_t *)message->attribute.data.value;
        zigbee_bridge_report_temp_humidity_to_matter(short_addr, message->src_endpoint,
                                                     ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, raw);
    } else if (message->cluster == 0x0405 &&
               message->attribute.id == 0x0000 &&
               message->attribute.data.size == 2) {
        int16_t raw = *(int16_t *)message->attribute.data.value;
        zigbee_bridge_report_temp_humidity_to_matter(short_addr, message->src_endpoint,
                                                     0x0405, raw);
    }
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        return zb_set_attr_value_handler((esp_zb_zcl_set_attr_value_message_t *)message);
    case ESP_ZB_CORE_REPORT_ATTR_CB_ID:
        return zb_report_attr_handler((esp_zb_zcl_report_attr_message_t *)message);
    default:
        return ESP_OK;
    }
}

static void zboss_task(void *pvParameters)
{
    /* initialize Zigbee stack with Zigbee coordinator config */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();
    zb_nwk_cfg.nwk_cfg.zczr_cfg.max_children = 16;
    esp_zb_secur_set_unsecure_tc_rejoin_enabled(true);
    esp_zb_secur_tcpol_set_allow_tc_rejoins(1);
    esp_zb_init(&zb_nwk_cfg);

    /* 注册一个 on/off switch 端点（含 on/off client），并附加 temperature/humidity client cluster，用于接收子设备报告 */
    esp_zb_on_off_switch_cfg_t switch_cfg = ESP_ZB_DEFAULT_ON_OFF_SWITCH_CONFIG();
    esp_zb_cluster_list_t *cluster_list = esp_zb_on_off_switch_clusters_create(&switch_cfg);

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
    esp_zb_core_action_handler_register(zb_action_handler);

    /* initiate Zigbee Stack start without zb_send_no_autostart_signal auto-start */
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_main_loop_iteration();
}

/* 按键检测：每 10ms 由 esp_timer 调用，不占用独立任务栈 */
static void button_timer_cb(void *arg)
{
    static bool stable_state = false;
    static int stable_count = 0;
    static bool was_pressed = false;
    static int press_start = 0;
    static uint32_t last_blink = 0;
    static bool led_state = false;

    int now = (int)(esp_timer_get_time() / 1000ULL);

    /* 入网期间 LED 间隔 500ms 闪烁，超时后自动停止 */
    if (steering_active) {
        if ((now - steering_start_time) >= (STEERING_DURATION_SEC * 1000)) {
            stop_steering();
            led_state = false;
            ESP_LOGI(TAG, "入网窗口 %d 秒已到期，停止入网", STEERING_DURATION_SEC);
        } else {
            if (now - (int)last_blink >= LED_BLINK_INTERVAL_MS) {
                led_state = !led_state;
                gpio_set_level(LED_GPIO, led_state);
                last_blink = now;
            }
        }
    }

    /* 读取原始电平并做稳定防抖（连续 3 次 10ms 相同才确认） */
    bool raw = (gpio_get_level(BUTTON_GPIO) == 0);
    if (raw == stable_state) {
        stable_count = 0;
    } else {
        stable_count++;
        if (stable_count >= 3) {
            stable_state = raw;
            stable_count = 0;

            if (stable_state && !was_pressed) {
                press_start = now;
                was_pressed = true;
                ESP_LOGI(TAG, "Button pressed (GPIO%d=0), network_formed=%d, steering_active=%d", BUTTON_GPIO, network_formed, steering_active);
            } else if (!stable_state && was_pressed) {
                int held_ms = now - press_start;
                was_pressed = false;
                if (held_ms >= DEBOUNCE_MS) {
                    if (!network_formed) {
                        ESP_LOGI(TAG, "按键释放 (%dms)，但网络尚未组建", held_ms);
                    } else if (steering_active) {
                        ESP_LOGI(TAG, "短按触发 (%dms)！正在关闭入网...", held_ms);
                        stop_steering();
                    } else {
                        ESP_LOGI(TAG, "短按触发 (%dms)！正在开放入网 %d 秒...", held_ms, STEERING_DURATION_SEC);
                        /* 通过 ZBOSS scheduler 在 ZBOSS 任务上下文中执行 BDB steering */
                        esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 0);
                        steering_active = true;
                        steering_start_time = now;
                    }
                } else {
                    ESP_LOGI(TAG, "按键释放，按压太短 (%dms)，忽略", held_ms);
                }
            }
        }
    }
}

void launch_app_zboss(void)
{
    /* 配置按键 GPIO (低电平有效，内部上拉) */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_conf));

    /* 配置 LED GPIO (输出) */
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_conf));
    gpio_set_level(LED_GPIO, 0);

    ESP_LOGI(TAG, "Button/LED init done, GPIO%d initial level: %d", BUTTON_GPIO, gpio_get_level(BUTTON_GPIO));

    /* 创建 10ms 周期 timer 检测按键，不占用独立任务栈 */
    const esp_timer_create_args_t timer_args = {
        .callback = &button_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "button_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t button_timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &button_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(button_timer, 10000)); /* 10ms */

    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    xTaskCreate(zboss_task, "zboss_main", 10240, xTaskGetCurrentTaskHandle(), 5, NULL);
}
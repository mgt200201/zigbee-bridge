/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zboss_manager.cpp
 *
 * @brief Zigbee 管理层 Facade 实现
 *
 * 本文件仅负责三件事：
 *   1. esp_zb_app_signal_handler — ZBOSS 信号回调入口，委托子模块处理
 *   2. zboss_task                 — ZBOSS 主任务初始化与事件循环
 *   3. zboss_manager_start        — 启动编排，创建任务 + 启动定时器
 *
 * 所有领域逻辑已拆分到：
 *   - zigbee_network.cpp   (网络状态 + Steering)
 *   - zigbee_device_db.cpp (设备数据库 + 端点映射)
 */

#include "zboss_manager.h"
#include "bridge_manager.h"
#include "zdp_discovery.h"
#include "watchdog.h"
#include <esp_log.h>
#include <esp_system.h>
#include <esp_zigbee_core.h>
#include <esp_zigbee_secur.h>
#include <ha/esp_zigbee_ha_standard.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "zboss_mgr";

/* ==================== ZBOSS 信号处理 ==================== */

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = static_cast<esp_zb_app_signal_type_t>(*p_sg_p);

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
            ESP_LOGI(TAG, "Formed network successfully (PAN: 0x%04hx)", esp_zb_get_pan_id());

            uint8_t network_key[16] = {0};
            if (esp_zb_secur_primary_network_key_get(network_key) == ESP_OK) {
                ESP_LOGI(TAG, "Network Key (for Wireshark decryption):");
                ESP_LOG_BUFFER_HEX(TAG, network_key, 16);
            }
            zigbee_net_set_formed(true);
        } else {
            ESP_LOGI(TAG, "Restart network formation (status: %d)", err_status);
            esp_zb_scheduler_alarm(
                (esp_zb_callback_t)esp_zb_bdb_start_top_level_commissioning,
                ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "BDB steering success");
        } else {
            ESP_LOGW(TAG, "BDB steering not successful (status: %d), retry", err_status);
            if (zigbee_net_is_steering()) {
                esp_zb_scheduler_alarm(
                    (esp_zb_callback_t)esp_zb_bdb_start_top_level_commissioning,
                    ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
            }
        }
        break;

    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *params =
            (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        ESP_LOGI(TAG, "New device commissioned or rejoined (short: 0x%04hx)", params->device_short_addr);
        zigbee_db_add_or_update(params->ieee_addr, params->device_short_addr);
        zdp_discovery_handle_device_annce(params->device_short_addr, params->ieee_addr,
                                          zigbee_net_is_steering());
        break;
    }

    case ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        esp_zb_zdo_signal_leave_indication_params_t *leave =
            (esp_zb_zdo_signal_leave_indication_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        ESP_LOGI(TAG, "Device leaving (short: 0x%04hx, rejoin: %d)", leave->short_addr, leave->rejoin);
        if (!leave->rejoin) {
            zdp_discovery_handle_device_leave(leave->short_addr, leave->device_addr);
        }
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
    (void)pvParameters;

    /* 注：zboss_main 不订阅 TWDT。
     * ZBOSS 主循环 esp_zb_main_loop() 是阻塞的，无法周期性喂狗。
     * 对 zigbee 栈的健康监控应使用上层心跳信号机制，而非硬件 WDT。 */

    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();
    zb_nwk_cfg.nwk_cfg.zczr_cfg.max_children = 16;

    /* 缩减 ZBOSS 缓冲区，释放堆内存给 Matter 栈
     * 原默认值: io=80, queue=80, network=64 — 150KB DRAM 无法容纳
     * network_size=32 保证 coordinator address table 足以处理 rejoin */
    esp_zb_io_buffer_size_set(48);
    esp_zb_scheduler_queue_size_set(48);
    esp_zb_overall_network_size_set(32);

    esp_zb_init(&zb_nwk_cfg);

    /* 栈就绪：此后 Matter 下行属性更新才允许发送 Zigbee 命令 */
    bridge_manager_set_zb_stack_ready(true);

    /* 注：esp_zb_nwk_set_ed_timeout / esp_zb_nwk_set_leave_req_allowed
     *     等为 test-utils 私有 API，当前版本已移除。安全策略使用 ZBOSS 默认值。 */

    /* 创建协调器端点（Basic + Identify Server） */
    esp_zb_on_off_switch_cfg_t switch_cfg = ESP_ZB_DEFAULT_ON_OFF_SWITCH_CONFIG();
    esp_zb_cluster_list_t *cluster_list = esp_zb_on_off_switch_clusters_create(&switch_cfg);

    esp_zb_endpoint_config_t ep_config = {
        .endpoint = HA_GW_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID,
        .app_device_version = 1,
    };
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_config);
    esp_zb_device_register(ep_list);

    esp_zb_core_action_handler_register(zdp_discovery_get_action_handler());
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
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

    zigbee_net_init_timer();
    ESP_LOGI(TAG, "ZBOSS manager started");
}
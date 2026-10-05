/*
   RCP 测试程序 - 验证 ESP32-H2 RCP 是否正常工作
   只测试 Zigbee RCP 通信，不包含 Matter
*/

#include <esp_log.h>
#include <esp_err.h>
#include <nvs_flash.h>
#include <esp_zigbee_core.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "rcp_test";

/* RCP UART 引脚配置（与主项目一致）*/
/* S3 GPIO4 是 RX，接 H2 的 TX */
/* S3 GPIO5 是 TX，接 H2 的 RX */
#define RCP_UART_RX_PIN    4
#define RCP_UART_TX_PIN    5
#define RCP_UART_PORT      UART_NUM_1
#define RCP_UART_BAUDRATE  460800

#define MAX_CHILDREN                32
#define ESP_ZB_PRIMARY_CHANNEL_MASK (1l << 13)

/* RCP 测试结果标志 */
static bool rcp_comm_ok = false;
static bool rcp_network_ok = false;

/* Zigbee 信号处理 */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)(*p_sg_p);

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        /* 到这一步说明 UART 与 RCP 通信成功 */
        rcp_comm_ok = true;
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "[步骤1] RCP 通信成功!");
        ESP_LOGI(TAG, "  -> UART 与 ESP32-H2 RCP 已建立连接");
        ESP_LOGI(TAG, "  -> Zigbee 协议栈已初始化");
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "[步骤2] 开始组建 Zigbee 网络...");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
        } else {
            ESP_LOGE(TAG, "");
            ESP_LOGE(TAG, "========================================");
            ESP_LOGE(TAG, "[失败] RCP 初始化失败! (status: %d)", err_status);
            ESP_LOGE(TAG, "========================================");
            ESP_LOGE(TAG, "请检查以下几项:");
            ESP_LOGE(TAG, "  1. TX/RX 是否交叉连接?");
            ESP_LOGE(TAG, "     S3 GPIO4(RX) -> H2 TX");
            ESP_LOGE(TAG, "     S3 GPIO5(TX) -> H2 RX");
            ESP_LOGE(TAG, "  2. H2 是否已烧录 ot_rcp 固件?");
            ESP_LOGE(TAG, "  3. GND 是否共地?");
            ESP_LOGE(TAG, "  4. H2 是否已上电运行?");
            ESP_LOGE(TAG, "========================================");
            ESP_LOGE(TAG, "");
        }
        break;

    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            rcp_network_ok = true;
            esp_zb_ieee_addr_t ieee_address;
            esp_zb_get_long_address(ieee_address);
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "#######################################");
            ESP_LOGI(TAG, "#                                     #");
            ESP_LOGI(TAG, "#   RCP 测试全部通过!                 #");
            ESP_LOGI(TAG, "#                                     #");
            ESP_LOGI(TAG, "#######################################");
            ESP_LOGI(TAG, "[步骤3] Zigbee 网络组建成功!");
            ESP_LOGI(TAG, "  IEEE 地址: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     ieee_address[7], ieee_address[6], ieee_address[5], ieee_address[4],
                     ieee_address[3], ieee_address[2], ieee_address[1], ieee_address[0]);
            ESP_LOGI(TAG, "  PAN ID:    0x%04hx", esp_zb_get_pan_id());
            ESP_LOGI(TAG, "  信道:      %d", esp_zb_get_current_channel());
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "结论: ESP32-H2 RCP 完全正常工作!");
            ESP_LOGI(TAG, "#######################################");
            ESP_LOGI(TAG, "");
        } else {
            ESP_LOGE(TAG, "[步骤3] 组网失败 (status: %d)，1秒后重试...", err_status);
            esp_zb_scheduler_alarm((esp_zb_callback_t)esp_zb_bdb_start_top_level_commissioning,
                                   ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "[步骤4] 网络已开放，允许设备入网");
        }
        break;

    default:
        ESP_LOGI(TAG, "[信号] status: %d, type: %d", err_status, sig_type);
        break;
    }
}

static void zboss_task(void *pvParameters)
{
    /* 初始化 Zigbee 协调器配置 */
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg = {
            .zczr_cfg = {
                .max_children = MAX_CHILDREN,
            },
        },
    };

    esp_zb_init(&zb_nwk_cfg);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_main_loop_iteration();
}

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "#######################################");
    ESP_LOGI(TAG, "#   ESP32-H2 RCP 测试程序             #");
    ESP_LOGI(TAG, "#######################################");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "硬件连接:");
    ESP_LOGI(TAG, "  ESP32-S3 GPIO4 (RX) <---> ESP32-H2 TX");
    ESP_LOGI(TAG, "  ESP32-S3 GPIO5 (TX) <---> ESP32-H2 RX");
    ESP_LOGI(TAG, "  ESP32-S3 GND        <---> ESP32-H2 GND");
    ESP_LOGI(TAG, "  UART 波特率: %d", RCP_UART_BAUDRATE);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "测试流程:");
    ESP_LOGI(TAG, "  步骤1: 验证 UART 与 RCP 通信");
    ESP_LOGI(TAG, "  步骤2: 初始化 Zigbee 协调器");
    ESP_LOGI(TAG, "  步骤3: 组建 Zigbee 网络 (验证射频)");
    ESP_LOGI(TAG, "");

    /* 初始化 NVS */
    nvs_flash_init();

    /* 配置 Zigbee 平台 - UART RCP 模式 */
    esp_zb_platform_config_t config = {
        .radio_config = {
            .radio_mode = ZB_RADIO_MODE_UART_RCP,
            .radio_uart_config = {
                .port = RCP_UART_PORT,
                .rx_pin = (gpio_num_t)RCP_UART_RX_PIN,
                .tx_pin = (gpio_num_t)RCP_UART_TX_PIN,
                .uart_config = {
                    .baud_rate = RCP_UART_BAUDRATE,
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

    esp_err_t err = esp_zb_platform_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "平台配置失败: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "正在与 RCP 建立通信...");
    ESP_LOGI(TAG, "(如果 10 秒内无响应，请检查接线和 RCP 固件)");
    ESP_LOGI(TAG, "");

    xTaskCreate(zboss_task, "zboss_main", 10240, NULL, 5, NULL);
}

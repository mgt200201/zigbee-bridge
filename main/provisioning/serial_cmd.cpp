/*
   SPDX-License-Identifier: Apache-2.0

   串口命令行实现。

   注意：CHIP shell console 与这里的 serial_cmd 任务共享 UART0 会导致 spinlock
   冲突，因此 app_init 中不再初始化 esp_matter::console，统一由本模块处理串口命令。
*/

#include "serial_cmd.h"

#include <stdio.h>
#include <string.h>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "board_config.h"
#include "device_info.h"
#include "rcp_coprocessor.h"

static const char *TAG = "serial_cmd";

#define SERIAL_CMD_TASK_STACK   4096
#define SERIAL_CMD_TASK_PRIO    2
#define SERIAL_CMD_LINE_LEN     128
#define SERIAL_CMD_POLL_MS      100

/* 清空 Matter 桥接端点 NVS 分区 */
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

/* factoryreset：清桥接 NVS + 设备信息，然后触发 Matter 恢复出厂 */
static void cmd_factory_reset(void)
{
    ESP_LOGI(TAG, "Factory reset triggered via serial...");
    gateway_erase_bridge_nvs();
    device_info_clear_all();
    esp_matter::factory_reset();
}

/* getIp：遍历所有 netif 打印 ifkey / up-down / IPv4。
 * 不依赖固定 ifkey（esp_netif_get_handle_from_ifkey）；日志用英文，避免串口中文乱码。 */
static void cmd_print_ip(void)
{
    esp_netif_t *netif = NULL;
    bool found = false;
    while ((netif = esp_netif_next(netif)) != NULL) {
        esp_netif_ip_info_t ip_info = {};
        esp_netif_get_ip_info(netif, &ip_info);
        const char *key = esp_netif_get_ifkey(netif);
        int up = esp_netif_is_netif_up(netif) ? 1 : 0;
        if (ip_info.ip.addr != 0) {
            ESP_LOGI(TAG, "Gateway IP [%s] up=%d: " IPSTR "  mask " IPSTR "  gw " IPSTR,
                     key, up,
                     IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
            found = true;
        } else {
            ESP_LOGW(TAG, "netif [%s] up=%d: no IPv4", key, up);
        }
    }
    if (!found) {
        ESP_LOGW(TAG, "Gateway IP: not available (no netif has IPv4 - check WiFi)");
    }
}

/* entest / bootest：H2 的 EN/BOOT 引脚电平翻转测试（万用表量电平用） */
static void cmd_gpio_toggle(bool boot)
{
    const gpio_num_t pin = boot ? RCP_BOOT_GPIO : RCP_EN_GPIO;
    const char *name = boot ? "BOOT" : "EN";

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
}

/* dltest：BOOT 拉低后脉冲 EN，保持 15s。H2 若进 ROM，Zigbee 会一直断。 */
static void cmd_download_test(void)
{
    gpio_config_t io = {};
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << RCP_EN_GPIO) | (1ULL << RCP_BOOT_GPIO);
    gpio_config(&io);

    gpio_set_level(RCP_BOOT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(RCP_EN_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(RCP_EN_GPIO, 1);
    ESP_LOGI(TAG, "dltest: BOOT=GPIO%d LOW, EN pulsed. Hold 15s", (int)RCP_BOOT_GPIO);
    vTaskDelay(pdMS_TO_TICKS(15000));
    gpio_set_level(RCP_BOOT_GPIO, 1);
    gpio_set_level(RCP_EN_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(RCP_EN_GPIO, 1);
    ESP_LOGI(TAG, "dltest done, H2 released to run");
}

/* iostest：S3 侧与 H2 相连引脚的悬空/上拉/下拉读数测试 */
static void cmd_io_test(void)
{
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
}

static void cmd_sync_test(void)
{
    ESP_LOGI(TAG, "synctest: connecting H2 ROM (Zigbee UART stolen, will reboot)");
    esp_err_t err = rcp_coprocessor_sync_test();
    ESP_LOGI(TAG, "synctest result: %s", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void serial_cmd_dispatch(char *line)
{
    if (strcmp(line, "factoryreset") == 0) {
        cmd_factory_reset();
    } else if (strcmp(line, "getIp") == 0) {
        cmd_print_ip();
    } else if (strcmp(line, "entest") == 0) {
        cmd_gpio_toggle(false);
    } else if (strcmp(line, "bootest") == 0) {
        cmd_gpio_toggle(true);
    } else if (strcmp(line, "dltest") == 0) {
        cmd_download_test();
    } else if (strcmp(line, "iostest") == 0) {
        cmd_io_test();
    } else if (strcmp(line, "synctest") == 0) {
        cmd_sync_test();
    } else if (strcmp(line, "help") == 0) {
        ESP_LOGI(TAG, "Commands: factoryreset, getIp, entest, bootest, dltest, iostest, synctest, help");
    } else if (strlen(line) > 0) {
        ESP_LOGI(TAG, "Unknown command: '%s' (try 'help')", line);
    }
}

static void serial_cmd_task(void *arg)
{
    (void)arg;
    char line[SERIAL_CMD_LINE_LEN];

    while (true) {
        if (fgets(line, sizeof(line), stdin)) {
            char *end = line + strlen(line);
            while (end > line && (*(end - 1) == '\n' || *(end - 1) == '\r')) {
                *(--end) = '\0';
            }
            serial_cmd_dispatch(line);
        }
        vTaskDelay(pdMS_TO_TICKS(SERIAL_CMD_POLL_MS));
    }
}

void serial_cmd_start(void)
{
    if (xTaskCreate(serial_cmd_task, "serial_cmd", SERIAL_CMD_TASK_STACK,
                    NULL, SERIAL_CMD_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial_cmd task, factoryreset unavailable");
    }
}

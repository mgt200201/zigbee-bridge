/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file self_test.cpp
 *
 * @brief 启动自检实现
 */

#include "self_test.h"
#include <esp_log.h>
#include <driver/gpio.h>
#include "board_config.h"

static const char *TAG = "self_test";

/* ==================== 硬件自检 ==================== */

/**
 * @brief 检测指定 GPIO 是否可以正常输出
 *
 * 策略：拉高 → 读电平 → 拉低 → 读电平 → 恢复
 * 注意：如果 GPIO 被外部电路拉死（短路），检测会失败。
 */
static esp_err_t gpio_toggle_test(gpio_num_t gpio, const char *name)
{
    esp_err_t ret;

    /* 配置为推挽输出 */
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, 1);
    esp_rom_delay_us(100);
    int level_high = gpio_get_level(gpio);

    gpio_set_level(gpio, 0);
    esp_rom_delay_us(100);
    int level_low = gpio_get_level(gpio);

    /* 恢复默认 */
    gpio_reset_pin(gpio);

    if (level_high == 1 && level_low == 0) {
        ESP_LOGD(TAG, "  [OK] %s (GPIO_%d): toggle passed", name, gpio);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "  [FAIL] %s (GPIO_%d): high=%d low=%d (expected 1/0)",
             name, gpio, level_high, level_low);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t self_test_hardware(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Hardware Self-Test                   ║");
    ESP_LOGI(TAG, "╠══════════════════════════════════════╣");

    esp_err_t result = ESP_OK;

    /* LED 检测 (status LED) */
    if (gpio_toggle_test(STATUS_LED_GPIO, "LED") != ESP_OK) {
        result = ESP_ERR_INVALID_STATE;
    }

    /* 按键检测：只检测 GPIO 是否可读，不模拟按下 */
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_GPIO, GPIO_PULLUP_ONLY);
    int btn_level = gpio_get_level(BUTTON_GPIO);
    ESP_LOGD(TAG, "  [INFO] Button (GPIO_%d): level=%d (1=released with pullup)", BUTTON_GPIO, btn_level);
    gpio_reset_pin(BUTTON_GPIO);

    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");
    return result;
}

/* ==================== 配置自检 ==================== */

esp_err_t self_test_config(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Configuration Validation             ║");
    ESP_LOGI(TAG, "╠══════════════════════════════════════╣");

    esp_err_t result = ESP_OK;
    bool valid = true;

    /* 1. ANNCE_DELAY 不能超过 RETRY_BASE（否则重试会在首次探测前触发） */
#if CONFIG_ZIGBEE_ANNCE_DELAY_MS >= CONFIG_ZIGBEE_DISCOVERY_RETRY_BASE_MS
    ESP_LOGW(TAG, "  [WARN] ZIGBEE_ANNCE_DELAY (%dms) >= RETRY_BASE (%dms): "
                  "retry may fire before probe completes",
             CONFIG_ZIGBEE_ANNCE_DELAY_MS,
             CONFIG_ZIGBEE_DISCOVERY_RETRY_BASE_MS);
    /* 这是性能问题不是错误，不标记失败 */
#endif

    /* 2. MAX_HANDLERS 太少 */
    if (CONFIG_ZIGBEE_MAX_HANDLERS < 8) {
        ESP_LOGE(TAG, "  [FAIL] ZIGBEE_MAX_HANDLERS=%d < 8 (minimum viable)",
                 CONFIG_ZIGBEE_MAX_HANDLERS);
        valid = false;
    } else {
        ESP_LOGI(TAG, "  [OK]  ZIGBEE_MAX_HANDLERS=%d", CONFIG_ZIGBEE_MAX_HANDLERS);
    }

    /* 3. DISCOVERY_MAX_RETRIES 和 RETRY_BASE 一致性 */
    if (CONFIG_ZIGBEE_DISCOVERY_MAX_RETRIES > 0 &&
        CONFIG_ZIGBEE_DISCOVERY_RETRY_BASE_MS < 200) {
        ESP_LOGW(TAG, "  [WARN] RETRY_BASE=%dms is very low, may cause rapid retries",
                 CONFIG_ZIGBEE_DISCOVERY_RETRY_BASE_MS);
    }

    /* 4. MAX_ENDPOINT_PROBE 边界 */
    if (CONFIG_ZIGBEE_MAX_ENDPOINT_PROBE < 1) {
        ESP_LOGE(TAG, "  [FAIL] ZIGBEE_MAX_ENDPOINT_PROBE=%d (must be >= 1)",
                 CONFIG_ZIGBEE_MAX_ENDPOINT_PROBE);
        valid = false;
    } else {
        ESP_LOGI(TAG, "  [OK]  ZIGBEE_MAX_ENDPOINT_PROBE=%d", CONFIG_ZIGBEE_MAX_ENDPOINT_PROBE);
    }

    if (!valid) {
        result = ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "╚══════════════════════════════════════╝");
    return result;
}

/* ==================== 全量自检 ==================== */

esp_err_t self_test_run(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Self-Test Started");
    ESP_LOGI(TAG, "============================================");

    esp_err_t ret_hw = self_test_hardware();
    esp_err_t ret_cfg = self_test_config();

    if (ret_hw == ESP_OK && ret_cfg == ESP_OK) {
        ESP_LOGI(TAG, "Self-test: ALL PASSED");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Self-test: hardware=%s, config=%s",
             esp_err_to_name(ret_hw),
             esp_err_to_name(ret_cfg));
    return ESP_ERR_INVALID_STATE;
}
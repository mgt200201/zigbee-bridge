/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "led.h"
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "led";

/** LED GPIO */
static gpio_num_t s_gpio = GPIO_NUM_NC;
/** true = 低电平点亮 */
static bool s_active_low = false;
/** 闪烁定时器句柄 */
static esp_timer_handle_t s_blink_timer = NULL;
/** 当前是否点亮（逻辑状态，与 GPIO 实际电平可能相反） */
static bool s_on = false;
/** 当前是否在闪烁 */
static bool s_blinking = false;

/**
 * @brief 将逻辑状态转换为 GPIO 电平
 */
static inline int logical_to_level(bool on)
{
    return s_active_low ? (on ? 0 : 1) : (on ? 1 : 0);
}

void led_on(void)
{
    s_on = true;
    gpio_set_level(s_gpio, logical_to_level(true));
}

void led_off(void)
{
    s_on = false;
    gpio_set_level(s_gpio, logical_to_level(false));
}

void led_toggle(void)
{
    s_on = !s_on;
    gpio_set_level(s_gpio, logical_to_level(s_on));
}

/**
 * @brief 闪烁定时器回调：每次触发翻转 LED 状态
 */
static void blink_timer_cb(void *arg)
{
    (void)arg;
    led_toggle();
}

esp_err_t led_init(gpio_num_t gpio, bool active_low)
{
    s_gpio = gpio;
    s_active_low = active_low;

    /* 配置 LED GPIO 为推挽输出 */
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&led_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 初始熄灭 */
    led_off();
    ESP_LOGI(TAG, "LED initialized on GPIO%d, active_low=%d", gpio, active_low);
    return ESP_OK;
}

void led_blink_start(uint32_t interval_ms)
{
    if (s_blink_timer) {
        /* 如果已在闪烁，先停止再重新开始 */
        led_blink_stop();
    }

    const esp_timer_create_args_t timer_args = {
        .callback = &blink_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_blink",
        .skip_unhandled_events = true,
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_blink_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Blink timer create failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_timer_start_periodic(s_blink_timer, interval_ms * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Blink timer start failed: %s", esp_err_to_name(err));
        return;
    }

    s_blinking = true;
    ESP_LOGI(TAG, "LED blink started, interval=%u ms", interval_ms);
}

void led_blink_stop(void)
{
    if (s_blink_timer) {
        esp_timer_stop(s_blink_timer);
        esp_timer_delete(s_blink_timer);
        s_blink_timer = NULL;
    }
    s_blinking = false;
    led_off();
}

bool led_is_blinking(void)
{
    return s_blinking;
}

/* ---- 不对称(亮/灭不同时长)闪烁 ---- */

/** 当前 pattern 模式的亮/灭时长 */
static uint32_t s_pattern_on_ms = 100;
static uint32_t s_pattern_off_ms = 400;

/**
 * @brief pattern 定时器回调：单次触发，根据当前逻辑状态切换亮灭并重新武装
 */
static void pattern_timer_cb(void *arg)
{
    (void)arg;
    if (s_on) {
        led_off();
        if (s_pattern_off_ms > 0) {
            esp_timer_start_once(s_blink_timer, s_pattern_off_ms * 1000);
        }
    } else {
        led_on();
        if (s_pattern_on_ms > 0) {
            esp_timer_start_once(s_blink_timer, s_pattern_on_ms * 1000);
        }
    }
}

void led_blink_start_pattern(uint32_t on_ms, uint32_t off_ms)
{
    if (s_blink_timer) {
        /* 如果已在闪烁，先停止再重新开始 */
        led_blink_stop();
    }

    s_pattern_on_ms = on_ms;
    s_pattern_off_ms = off_ms;

    const esp_timer_create_args_t timer_args = {
        .callback = &pattern_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_blink_pattern",
        .skip_unhandled_events = true,
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_blink_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Pattern timer create failed: %s", esp_err_to_name(err));
        return;
    }

    /* 先点亮，on_ms 后进入灭→亮循环 */
    led_on();
    err = esp_timer_start_once(s_blink_timer, on_ms * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Pattern timer start failed: %s", esp_err_to_name(err));
        return;
    }

    s_blinking = true;
    ESP_LOGI(TAG, "LED pattern started: on=%u ms, off=%u ms", on_ms, off_ms);
}
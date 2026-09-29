/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "led.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <stdlib.h>

static const char *TAG = "led";

/** 单颗 LED 状态 */
struct led_ctx {
    gpio_num_t gpio;           /**< LED GPIO */
    bool active_low;           /**< true = 低电平点亮 */
    bool on;                   /**< 逻辑点亮状态（与 GPIO 电平可能相反） */
    bool blinking;             /**< 是否在闪烁 */
    bool pattern_mode;         /**< true = 不对称 pattern 闪烁 */
    uint32_t pattern_on_ms;    /**< pattern 点亮时长 */
    uint32_t pattern_off_ms;   /**< pattern 熄灭时长 */
    esp_timer_handle_t timer;  /**< 闪烁定时器 */
};

/** 将逻辑状态转换为 GPIO 电平 */
static inline int logical_to_level(led_handle_t led, bool on)
{
    return led->active_low ? (on ? 0 : 1) : (on ? 1 : 0);
}

static void led_apply(led_handle_t led)
{
    gpio_set_level(led->gpio, logical_to_level(led, led->on));
}

void led_on(led_handle_t led)
{
    if (!led) {
        return;
    }
    led->on = true;
    led_apply(led);
}

void led_off(led_handle_t led)
{
    if (!led) {
        return;
    }
    led->on = false;
    led_apply(led);
}

void led_toggle(led_handle_t led)
{
    if (!led) {
        return;
    }
    led->on = !led->on;
    led_apply(led);
}

/**
 * @brief 对称闪烁定时器回调：每次触发翻转 LED 状态
 */
static void blink_timer_cb(void *arg)
{
    led_handle_t led = (led_handle_t)arg;
    led_toggle(led);
}

/**
 * @brief pattern 定时器回调：单次触发，根据当前状态切换亮灭并重新武装
 */
static void pattern_timer_cb(void *arg)
{
    led_handle_t led = (led_handle_t)arg;
    if (led->on) {
        led_off(led);
        if (led->pattern_off_ms > 0) {
            esp_timer_start_once(led->timer, led->pattern_off_ms * 1000);
        }
    } else {
        led_on(led);
        if (led->pattern_on_ms > 0) {
            esp_timer_start_once(led->timer, led->pattern_on_ms * 1000);
        }
    }
}

led_handle_t led_create(gpio_num_t gpio, bool active_low)
{
    led_handle_t led = (led_handle_t)calloc(1, sizeof(struct led_ctx));
    if (!led) {
        ESP_LOGE(TAG, "alloc failed");
        return NULL;
    }
    led->gpio = gpio;
    led->active_low = active_low;

    gpio_config_t conf = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&conf) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed (gpio=%d)", gpio);
        free(led);
        return NULL;
    }

    led_off(led);
    ESP_LOGI(TAG, "LED created on GPIO%d, active_low=%d", gpio, (int)active_low);
    return led;
}

void led_destroy(led_handle_t led)
{
    if (!led) {
        return;
    }
    led_blink_stop(led);
    gpio_reset_pin(led->gpio);
    free(led);
}

void led_blink_start(led_handle_t led, uint32_t interval_ms)
{
    if (!led) {
        return;
    }
    led_blink_stop(led);
    led->pattern_mode = false;

    const esp_timer_create_args_t args = {
        .callback = &blink_timer_cb,
        .arg = led,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_blink",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &led->timer) != ESP_OK) {
        ESP_LOGE(TAG, "Blink timer create failed");
        return;
    }
    if (esp_timer_start_periodic(led->timer, interval_ms * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "Blink timer start failed");
        return;
    }
    led->blinking = true;
    ESP_LOGI(TAG, "LED blink started, interval=%u ms", (unsigned int)interval_ms);
}

void led_blink_start_pattern(led_handle_t led, uint32_t on_ms, uint32_t off_ms)
{
    if (!led) {
        return;
    }
    led_blink_stop(led);
    led->pattern_mode = true;
    led->pattern_on_ms = on_ms;
    led->pattern_off_ms = off_ms;

    const esp_timer_create_args_t args = {
        .callback = &pattern_timer_cb,
        .arg = led,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_blink_pattern",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &led->timer) != ESP_OK) {
        ESP_LOGE(TAG, "Pattern timer create failed");
        return;
    }

    /* 先点亮，on_ms 后进入 灭→亮 循环 */
    led_on(led);
    if (esp_timer_start_once(led->timer, on_ms * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "Pattern timer start failed");
        return;
    }
    led->blinking = true;
    ESP_LOGI(TAG, "LED pattern started: on=%u ms, off=%u ms",
             (unsigned int)on_ms, (unsigned int)off_ms);
}

void led_blink_stop(led_handle_t led)
{
    if (!led) {
        return;
    }
    if (led->timer) {
        esp_timer_stop(led->timer);
        esp_timer_delete(led->timer);
        led->timer = NULL;
    }
    led->blinking = false;
    led->pattern_mode = false;
    led_off(led);
}

bool led_is_blinking(led_handle_t led)
{
    return led ? led->blinking : false;
}
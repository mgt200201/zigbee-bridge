/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "button.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

static const char *TAG = "button";

/** 默认长按判定时间（3秒） */
#define DEFAULT_LONG_PRESS_MS   3000
/** 防抖采样次数：连续 3 次相同电平才确认 */
#define DEBOUNCE_SAMPLES        3
/** 定时器周期（10ms） */
#define POLL_INTERVAL_MS        10
/** 防抖超时（30ms = 3次采样），短于此时间的抖动被忽略 */
#define DEBOUNCE_TIMEOUT_MS     30

/** 按键 GPIO */
static gpio_num_t s_gpio = GPIO_NUM_NC;
/** 按键事件回调 */
static button_callback_t s_callback = NULL;
/** 回调用户上下文 */
static void *s_user_data = NULL;
/** 长按阈值（毫秒） */
static uint32_t s_long_press_ms = DEFAULT_LONG_PRESS_MS;

/** 按键状态机变量（在定时器回调中访问） */
static bool s_stable_state = false;
static int s_stable_count = 0;
static bool s_was_pressed = false;
static int s_press_start = 0;

/**
 * @brief 按键定时器回调（每 10ms 由 esp_timer 调用）
 *
 * 实现以下功能：
 * 1. 按键状态检测：连续 3 次采样相同电平确认防抖
 * 2. 检测到稳定状态变化时触发事件：
 *    - 下降沿（按下）：记录按下开始时间
 *    - 上升沿（释放）：根据按压时长判定短按/长按，调用回调
 *
 * @param arg 定时器参数（未使用）
 */
static void button_timer_cb(void *arg)
{
    int now = (int)(esp_timer_get_time() / 1000ULL);

    /* 读取原始电平（低电平有效） */
    bool raw = (gpio_get_level(s_gpio) == 0);

    /* 防抖：连续 3 次采样相同才确认状态变化 */
    if (raw == s_stable_state) {
        s_stable_count = 0;
    } else {
        s_stable_count++;
        if (s_stable_count >= DEBOUNCE_SAMPLES) {
            s_stable_state = raw;
            s_stable_count = 0;

            if (s_stable_state && !s_was_pressed) {
                /* 按键按下 */
                s_press_start = now;
                s_was_pressed = true;
            } else if (!s_stable_state && s_was_pressed) {
                /* 按键释放 */
                int held_ms = now - s_press_start;
                s_was_pressed = false;

                if (held_ms >= DEBOUNCE_TIMEOUT_MS) {
                    button_event_t event = (held_ms >= (int)s_long_press_ms)
                                           ? BUTTON_EVENT_LONG_PRESS
                                           : BUTTON_EVENT_SHORT_PRESS;
                    if (s_callback) {
                        s_callback(event, s_user_data);
                    }
                }
            }
        }
    }
}

esp_err_t button_init(gpio_num_t gpio)
{
    s_gpio = gpio;

    /* 配置按键 GPIO（低电平有效，内部上拉） */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&btn_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 创建 10ms 周期定时器检测按键 */
    const esp_timer_create_args_t timer_args = {
        .callback = &button_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "button_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer;
    err = esp_timer_create(&timer_args, &timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Timer create failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(timer, POLL_INTERVAL_MS * 1000); /* 微秒 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Timer start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Button initialized on GPIO%d, initial level=%d",
             gpio, gpio_get_level(gpio));
    return ESP_OK;
}

esp_err_t button_register_callback(button_callback_t cb, void *user_data)
{
    s_callback = cb;
    s_user_data = user_data;
    return ESP_OK;
}

void button_set_long_press_ms(uint32_t ms)
{
    s_long_press_ms = ms;
}
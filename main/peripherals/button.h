/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <driver/gpio.h>
#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按键事件类型
 */
typedef enum {
    BUTTON_EVENT_SHORT_PRESS = 0,   /**< 短按（< 长按阈值） */
    BUTTON_EVENT_LONG_PRESS,        /**< 长按（>= 长按阈值） */
} button_event_t;

/**
 * @brief 按键事件回调函数类型
 * @param event 按键事件
 * @param user_data 用户注册的上下文数据
 */
typedef void (*button_callback_t)(button_event_t event, void *user_data);

/**
 * @brief 初始化按键
 *
 * 配置 GPIO 为输入（内部上拉），启动 10ms 周期定时器做防抖检测。
 *
 * @param gpio 按键 GPIO 号（低电平有效）
 * @return esp_err_t
 */
esp_err_t button_init(gpio_num_t gpio);

/**
 * @brief 注册按键事件回调
 *
 * 当检测到有效的按键事件（短按/长按）时，回调函数会在定时器上下文中被调用。
 *
 * @param cb 回调函数指针
 * @param user_data 用户上下文，会透传给回调
 * @return esp_err_t
 */
esp_err_t button_register_callback(button_callback_t cb, void *user_data);

/**
 * @brief 设置长按判定阈值
 * @param ms 长按阈值（毫秒），默认 3000ms
 */
void button_set_long_press_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
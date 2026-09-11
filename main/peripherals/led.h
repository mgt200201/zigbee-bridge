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
 * @brief 初始化 LED
 *
 * 配置 GPIO 为推挽输出，初始状态为熄灭。
 *
 * @param gpio LED GPIO 号
 * @param active_low true = 低电平点亮，false = 高电平点亮
 * @return esp_err_t
 */
esp_err_t led_init(gpio_num_t gpio, bool active_low);

/** 点亮 LED */
void led_on(void);

/** 熄灭 LED */
void led_off(void);

/** 翻转 LED 状态 */
void led_toggle(void);

/**
 * @brief 开始以固定间隔闪烁
 * @param interval_ms 闪烁间隔（毫秒），如 500 表示 500ms 切换一次
 */
void led_blink_start(uint32_t interval_ms);

/** 停止闪烁 */
void led_blink_stop(void);

/** @return true 当前正在闪烁 */
bool led_is_blinking(void);

#ifdef __cplusplus
}
#endif
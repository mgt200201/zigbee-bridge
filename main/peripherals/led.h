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
 * @brief LED 实例句柄
 *
 * 每个 handle 代表一颗独立的 LED（独立的 GPIO、极性、亮灭状态与闪烁定时器），
 * 因此可驱动多颗行为不同的 LED，互不干扰。
 */
typedef struct led_ctx *led_handle_t;

/**
 * @brief 创建一颗 LED
 *
 * 配置 GPIO 为推挽输出，初始熄灭。
 *
 * @param gpio LED GPIO 号
 * @param active_low true = 低电平点亮，false = 高电平点亮
 * @return LED 句柄；失败返回 NULL
 */
led_handle_t led_create(gpio_num_t gpio, bool active_low);

/** @brief 释放 LED 资源（停止闪烁并删除定时器） */
void led_destroy(led_handle_t led);

/** 点亮 LED */
void led_on(led_handle_t led);

/** 熄灭 LED */
void led_off(led_handle_t led);

/** 翻转 LED 状态 */
void led_toggle(led_handle_t led);

/**
 * @brief 开始以固定间隔闪烁
 * @param led LED 句柄
 * @param interval_ms 闪烁间隔（毫秒），如 500 表示 500ms 切换一次
 */
void led_blink_start(led_handle_t led, uint32_t interval_ms);

/**
 * @brief 开始以不对称亮/灭时长闪烁（如 100ms 亮 / 400ms 灭）
 * @param led LED 句柄
 * @param on_ms 点亮时长（毫秒）
 * @param off_ms 熄灭时长（毫秒）
 */
void led_blink_start_pattern(led_handle_t led, uint32_t on_ms, uint32_t off_ms);

/** 停止闪烁 */
void led_blink_stop(led_handle_t led);

/** @return true 当前正在闪烁 */
bool led_is_blinking(led_handle_t led);

#ifdef __cplusplus
}
#endif
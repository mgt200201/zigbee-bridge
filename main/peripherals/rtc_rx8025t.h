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
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 RX8025T RTC（I2C 从机地址固定 0x32）
 *
 * @param scl_io       SCL 引脚
 * @param sda_io       SDA 引脚
 * @param scl_speed_hz I2C 时钟频率，如 100000（100kHz）
 * @return esp_err_t
 */
esp_err_t rtc_rx8025t_init(gpio_num_t scl_io, gpio_num_t sda_io, uint32_t scl_speed_hz);

/**
 * @brief 设置 RTC 时间
 *
 * @param time struct tm（tm_year = 实际年 - 1900，tm_mon = 实际月 - 1）
 * @return esp_err_t
 */
esp_err_t rtc_rx8025t_set_time(const struct tm *time);

/**
 * @brief 读取 RTC 时间
 *
 * @param time 输出 struct tm
 * @return esp_err_t
 */
esp_err_t rtc_rx8025t_get_time(struct tm *time);

#ifdef __cplusplus
}
#endif
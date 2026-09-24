/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "rtc_rx8025t.h"

#include <esp_log.h>
#include <driver/i2c_master.h>

static const char *TAG = "rtc_rx8025t";

/* RX8025T I2C 7-bit 地址（固定） */
#define RX8025T_ADDR 0x32

/* 时间/日期寄存器地址 */
#define RX8025T_REG_SEC  0x00
#define RX8025T_REG_MIN  0x01
#define RX8025T_REG_HOUR 0x02
#define RX8025T_REG_WEEK 0x03
#define RX8025T_REG_DAY  0x04
#define RX8025T_REG_MON  0x05
#define RX8025T_REG_YEAR 0x06

static i2c_master_dev_handle_t s_dev = NULL;

static inline uint8_t bin_to_bcd(uint8_t v)
{
    return ((v / 10) << 4) | (v % 10);
}

static inline uint8_t bcd_to_bin(uint8_t v)
{
    return ((v >> 4) * 10) + (v & 0x0F);
}

esp_err_t rtc_rx8025t_init(gpio_num_t scl_io, gpio_num_t sda_io, uint32_t scl_speed_hz)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_io,
        .scl_io_num = scl_io,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RX8025T_ADDR,
        .scl_speed_hz = scl_speed_hz,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "RX8025T initialized (SCL=%d, SDA=%d, speed=%lu Hz)",
             scl_io, sda_io, (unsigned long)scl_speed_hz);
    return ESP_OK;
}

esp_err_t rtc_rx8025t_set_time(const struct tm *time)
{
    if (!s_dev || !time) {
        return ESP_ERR_INVALID_STATE;
    }

    /* bit7 = 世纪位 C，>= 2000 年置 1 */
    uint8_t mon = bin_to_bcd((uint8_t)(time->tm_mon + 1));
    if ((time->tm_year + 1900) >= 2000) {
        mon |= 0x80;
    }

    uint8_t buf[8];
    buf[0] = RX8025T_REG_SEC;
    buf[1] = bin_to_bcd((uint8_t)time->tm_sec);
    buf[2] = bin_to_bcd((uint8_t)time->tm_min);
    buf[3] = bin_to_bcd((uint8_t)time->tm_hour);
    buf[4] = (uint8_t)(time->tm_wday & 0x07);
    buf[5] = bin_to_bcd((uint8_t)time->tm_mday);
    buf[6] = mon;
    buf[7] = bin_to_bcd((uint8_t)(time->tm_year % 100));

    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_time transmit failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t rtc_rx8025t_get_time(struct tm *time)
{
    if (!s_dev || !time) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t reg = RX8025T_REG_SEC;
    uint8_t buf[7] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_time read failed: %s", esp_err_to_name(err));
        return err;
    }

    time->tm_sec  = (int)bcd_to_bin(buf[0] & 0x7F);
    time->tm_min  = (int)bcd_to_bin(buf[1] & 0x7F);
    time->tm_hour = (int)bcd_to_bin(buf[2] & 0x3F);
    time->tm_wday = (int)(buf[3] & 0x07);
    time->tm_mday = (int)bcd_to_bin(buf[4] & 0x3F);
    time->tm_mon  = (int)bcd_to_bin(buf[5] & 0x1F) - 1;
    /* 简化处理：默认 20xx 年 */
    time->tm_year = (int)bcd_to_bin(buf[6]) + 100;

    return ESP_OK;
}
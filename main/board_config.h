/*
   Board-level GPIO configuration.

   All board pins are defined here so that changing a pin on a PCB revision
   only requires editing these macros, not touching every usage site.
*/

#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"

/* 入网/按键：短按触发 Zigbee 入网窗口（GPIO9 = 旧版本） */
#define BUTTON_GPIO              GPIO_NUM_38

/* ---- 状态灯 LED（启动 100ms亮/400ms灭，就绪后常亮） ---- */
#define STATUS_LED_GPIO          GPIO_NUM_41
#define STATUS_LED_POLARITY      false   /* false=高电平点亮, true=低电平点亮 */
#define LED_BOOT_ON_MS           100     /* 启动未就绪：点亮时长(ms) */
#define LED_BOOT_OFF_MS          400     /* 启动未就绪：熄灭时长(ms) */

/* ---- 入网闪烁 LED（开放入网时 500ms 间隔闪烁） ---- */
#define STEERING_LED_GPIO        GPIO_NUM_42
#define STEERING_LED_POLARITY    false   /* false=高电平点亮, true=低电平点亮 */
#define STEERING_BLINK_MS        500     /* 入网窗口闪烁间隔(ms) */

/* ---- External RTC RX8025T-UB (I2C master, 7-bit addr 0x32) ----
 * 板级实际走线（见原理图 "RTC时钟"）：SCL = S3 IO13, SDA = S3 IO14。
 * 板上已有 R5/R7 10K 上拉至 VDD3V3；INT# 经 R8 10K 上拉至 VDD3V3_BAT，未接 MCU。
 * 若将来改板只需要改这两个宏。 */
#define RTC_I2C_SCL_GPIO         GPIO_NUM_13
#define RTC_I2C_SDA_GPIO         GPIO_NUM_14
#define RTC_I2C_SPEED_HZ         100000   /* 100kHz */

/* ---- ESP32-H2 RCP 控制脚（与 Kconfig PIN_TO_RCP_* 一致） ----
 * 平时：EN=高、BOOT=高 → H2 正常跑固件
 * OTA：serial-flasher 先拉低 BOOT，再脉冲 EN，进 UART 下载模式
 * 注意：GPIO46 在部分 ESP32-S3 上为 input-only，若 OTA 无法复位 H2 请换脚 */
#define RCP_EN_GPIO              ((gpio_num_t)CONFIG_PIN_TO_RCP_RESET)  /* S3 IO46 → H2 EN */
#define RCP_BOOT_GPIO            ((gpio_num_t)CONFIG_PIN_TO_RCP_BOOT)   /* S3 IO3  → H2 BOOT */
/*
   Board-level GPIO configuration.

   All board pins are defined here so that changing a pin on a PCB revision
   only requires editing these macros, not touching every usage site.
*/

#pragma once

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
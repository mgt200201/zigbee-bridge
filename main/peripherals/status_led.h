/*
   SPDX-License-Identifier: Apache-2.0

   网关状态 LED 状态机。

   状态优先级：
     - 开放入网（Zigbee steering 激活）：入网灯 500ms 对称闪烁
     - 已就绪（已配网 && Zigbee 网络就绪 && 检测到活跃 Matter 安全会话）：状态灯常亮
     - 未就绪：状态灯 100ms 亮 / 400ms 灭
*/

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建两颗状态 LED 并进入"未就绪"状态 */
void status_led_init(void);

/** @brief 更新 Zigbee 入网窗口状态（true=开放入网） */
void status_led_set_steering(bool active);

/** @brief 标记 Zigbee 协调器网络组建成功 */
void status_led_set_zigbee_ready(void);

/** @brief 启动就绪确认轮询（检测活跃 Matter 会话 → 常亮） */
void status_led_start_ready_poll(void);

#ifdef __cplusplus
}
#endif

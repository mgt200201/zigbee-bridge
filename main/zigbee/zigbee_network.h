/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zigbee_network.h
 *
 * @brief Zigbee 网络状态与 Steering 入网窗口管理
 *
 * 职责：
 *   - 网络组建状态追踪
 *   - Steering 入网窗口的开启/关闭/超时检测
 *   - Steering 状态变化回调（供 LED 反馈等）
 *   - 按键短按切换 Steering
 *
 * 原属于 zboss_manager.cpp，按大厂单一职责原则拆分。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 网络状态查询 ==================== */

bool zigbee_net_is_formed(void);

bool zigbee_net_is_steering(void);

bool zigbee_net_steering_expired(void);

/* ==================== Steering 控制 ==================== */

void zigbee_net_start_steering(void);

void zigbee_net_stop_steering(void);

/**
 * @brief 处理按键短按：切换 Steering 开关
 *
 * 网络未组建时无操作。
 */
void zigbee_net_handle_button(void);

/* ==================== Steering 回调 ==================== */

typedef void (*zigbee_net_steering_cb_t)(bool active);

void zigbee_net_register_steering_cb(zigbee_net_steering_cb_t cb);

/* ==================== 网络就绪回调 ==================== */

typedef void (*zigbee_net_ready_cb_t)(void);

/**
 * @brief 注册网络就绪回调：协调器 network formation 成功（网络可用）时触发
 */
void zigbee_net_register_ready_cb(zigbee_net_ready_cb_t cb);

/* ==================== 内部接口（仅 zboss_manager.cpp 调用） ==================== */

void zigbee_net_set_formed(bool formed);

void zigbee_net_init_timer(void);

#ifdef __cplusplus
}
#endif
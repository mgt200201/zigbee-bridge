/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zigbee_network.cpp
 *
 * @brief 网络状态 + Steering 入网窗口管理实现
 */

#include "zigbee_network.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_zigbee_core.h>

static const char *TAG = "zb_net";

#define STEERING_DURATION_SEC   180

/* ── 网络状态 ── */
static bool s_formed = false;

/* ── Steering 状态 ── */
static bool s_steering_active = false;
static int  s_steering_start_ms = 0;

/* ── Steering 回调 ── */
static zigbee_net_steering_cb_t s_steering_cb = NULL;

/* ── 网络就绪回调 ── */
static zigbee_net_ready_cb_t s_ready_cb = NULL;

/* ── 超时定时器 ── */
static esp_timer_handle_t s_steering_timer = NULL;

/* ── 前向声明 ── */
static void steering_timer_cb(void *arg);

/* ==================== 网络状态查询 ==================== */

bool zigbee_net_is_formed(void)
{
    return s_formed;
}

bool zigbee_net_is_steering(void)
{
    return s_steering_active;
}

bool zigbee_net_steering_expired(void)
{
    if (!s_steering_active) {
        return false;
    }
    int now = (int)(esp_timer_get_time() / 1000ULL);
    return (now - s_steering_start_ms) >= (STEERING_DURATION_SEC * 1000);
}

/* ==================== Steering 控制 ==================== */

void zigbee_net_start_steering(void)
{
    if (!s_formed) {
        ESP_LOGW(TAG, "Network not formed, cannot start steering");
        return;
    }
    s_steering_active = true;
    s_steering_start_ms = (int)(esp_timer_get_time() / 1000ULL);
    ESP_LOGI(TAG, "Steering started, duration=%d sec", STEERING_DURATION_SEC);

    if (s_steering_cb) {
        s_steering_cb(true);
    }

    esp_zb_scheduler_alarm(
        (esp_zb_callback_t)esp_zb_bdb_start_top_level_commissioning,
        ESP_ZB_BDB_MODE_NETWORK_STEERING, 0);
}

void zigbee_net_stop_steering(void)
{
    if (!s_steering_active) {
        return;
    }
    s_steering_active = false;
    s_steering_start_ms = 0;
    ESP_LOGI(TAG, "Steering stopped");

    if (s_steering_cb) {
        s_steering_cb(false);
    }
}

void zigbee_net_handle_button(void)
{
    if (!s_formed) {
        ESP_LOGI(TAG, "Network not formed, ignoring button");
        return;
    }
    if (zigbee_net_is_steering()) {
        zigbee_net_stop_steering();
    } else {
        zigbee_net_start_steering();
    }
}

/* ==================== Steering 回调 ==================== */

void zigbee_net_register_steering_cb(zigbee_net_steering_cb_t cb)
{
    s_steering_cb = cb;
}

void zigbee_net_register_ready_cb(zigbee_net_ready_cb_t cb)
{
    s_ready_cb = cb;
}

/* ==================== 超时定时器 ==================== */

static void steering_timer_cb(void *arg)
{
    (void)arg;
    if (zigbee_net_steering_expired()) {
        ESP_LOGI(TAG, "Steering timeout, closing window");
        zigbee_net_stop_steering();
    }
}

/* ==================== 内部：供 zboss_manager 调用 ==================== */

void zigbee_net_set_formed(bool formed)
{
    s_formed = formed;
    if (formed && s_ready_cb) {
        s_ready_cb(); /* 协调器网络组建成功 → 通知系统就绪 */
    }
}

bool zigbee_net_is_active(void)
{
    return s_steering_active;
}

void zigbee_net_init_timer(void)
{
    const esp_timer_create_args_t args = {
        .callback = &steering_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "steering_tmr",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_steering_timer) == ESP_OK) {
        esp_timer_start_periodic(s_steering_timer, 1000000);
        ESP_LOGI(TAG, "Steering timeout timer started");
    } else {
        ESP_LOGW(TAG, "Failed to create steering timeout timer");
    }
}
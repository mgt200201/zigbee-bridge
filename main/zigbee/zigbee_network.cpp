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

/* 在 ZBOSS 任务上下文执行：真正打开入网许可（permit join） */
static void zb_open_network_cb(uint8_t duration_sec)
{
    esp_err_t err = esp_zb_bdb_open_network(duration_sec);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Open network (%u s) failed: %s", duration_sec, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Network opened for %u s (permit join on)", duration_sec);
    }
}

/* 在 ZBOSS 任务上下文执行：真正关闭入网许可 */
static void zb_close_network_cb(uint8_t param)
{
    (void)param;
    esp_err_t err = esp_zb_bdb_close_network();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Close network failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Network closed (permit join off)");
    }
}

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
    /* 显式开放入网，时长与软件窗口一致（仅靠 steering 无法精确控制时长） */
    esp_zb_scheduler_alarm((esp_zb_callback_t)zb_open_network_cb, STEERING_DURATION_SEC, 0);
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

    /* 只改标志位不会关掉 ZBOSS 的 permit join，必须显式关闭网络，
     * 否则窗口"关闭"后新设备仍能在剩余时长内入网。 */
    esp_zb_scheduler_alarm((esp_zb_callback_t)zb_close_network_cb, 0, 0);
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
    if (formed) {
        /* 建网/重启后 ZBOSS 默认会开放 permit join 一段时间。
         * 这里主动关掉，保证只有用户短按开窗时才允许新设备入网。 */
        esp_zb_scheduler_alarm((esp_zb_callback_t)zb_close_network_cb, 0, 1000);
        if (s_ready_cb) {
            s_ready_cb(); /* 协调器网络组建成功 → 通知系统就绪 */
        }
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
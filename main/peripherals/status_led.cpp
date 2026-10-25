/*
   SPDX-License-Identifier: Apache-2.0

   网关状态 LED 状态机实现。

   就绪(常亮)采用"确认就绪"机制：周期轮询网关的活跃 Matter 安全会话，
   一旦检测到有控制器（HA）连上来，且 已配网(fabric) && Zigbee 网络组建成功
   两个基础条件也满足，就立即常亮——这是真实确认，无需猜计时。
   活跃的 CASE 安全会话本身就是"网关已就绪、此刻即可被控制"的最强证据，
   因此不依赖 kOperationalNetworkStarted 等可能不在重启/已配网路径触发的信号。
*/

#include "status_led.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <app/server/Server.h>
#include <transport/SecureSession.h>
#include <transport/Session.h>

#include "board_config.h"
#include "led.h"
#include "matter/app_status.h"

static const char *TAG = "status_led";

#define STATUS_READY_POLL_MS     2000UL

static volatile bool s_steering_active     = false; /* Zigbee 入网窗口是否开启 */
static volatile bool s_gateway_ready       = false; /* Matter 是否已配网(fabric) */
static volatile bool s_zigbee_ready        = false; /* Zigbee 协调器网络是否组建成功 */
static volatile bool s_confirmed_ready     = false; /* 已确认就绪（检测到活跃会话） */
static bool s_poll_timer_started           = false;
static esp_timer_handle_t s_ready_poll_timer = NULL;

/* 两颗独立 LED：
 *  - s_status_led   : 状态灯（启动 100/400 闪烁，就绪常亮）
 *  - s_steering_led : 入网闪烁灯（开放入网时 500ms 间隔闪烁） */
static led_handle_t s_status_led   = NULL;
static led_handle_t s_steering_led = NULL;

static void status_led_refresh(void);

/** 统计当前活跃的 Matter 安全会话数（HA 重连会自动建立 CASE 会话/订阅） */
static int active_secure_session_count(void)
{
    int count = 0;
    auto &mgr = chip::Server::GetInstance().GetSecureSessionManager();
    mgr.ForEachSessionHandle(&count, [](void *ctx, chip::SessionHandle &handle) -> bool {
        if (handle->IsActiveSession()) {
            ++(*(static_cast<int *>(ctx)));
        }
        return true; /* 继续遍历 */
    });
    return count;
}

/** 就绪确认轮询：检测到活跃会话且基础条件齐 → 常亮 */
static void ready_poll_cb(void *arg)
{
    (void)arg;
    if (!s_confirmed_ready && !s_steering_active &&
        s_gateway_ready && s_zigbee_ready &&
        active_secure_session_count() > 0) {
        s_confirmed_ready = true;
        ESP_LOGI(TAG, "Ready confirmed: active Matter session detected");
        status_led_refresh();
    }
}

/** 根据当前状态刷新两颗 LED。
 *  - 入网闪烁灯：开放入网时 500ms 闪烁，否则熄灭
 *  - 状态灯：就绪常亮，未就绪 100ms 亮/400ms 灭 */
static void status_led_refresh(void)
{
    if (s_steering_led) {
        if (s_steering_active) {
            led_blink_start(s_steering_led, STEERING_BLINK_MS);
        } else {
            led_blink_stop(s_steering_led);
        }
    }

    if (s_status_led) {
        if (s_confirmed_ready) {
            led_blink_stop(s_status_led);
            led_on(s_status_led);                       /* 就绪常亮 */
        } else {
            led_blink_start_pattern(s_status_led, LED_BOOT_ON_MS, LED_BOOT_OFF_MS);
        }
    }
}

/** 由 bridge_manager / app_init 调用：标记已配网(fabric)并刷新 LED */
extern "C" void app_status_notify_ready(void)
{
    s_gateway_ready = true;
    status_led_refresh();
}

void status_led_init(void)
{
    s_status_led   = led_create(STATUS_LED_GPIO,   STATUS_LED_POLARITY);
    s_steering_led = led_create(STEERING_LED_GPIO, STEERING_LED_POLARITY);
    status_led_refresh();         /* 刚上电：状态灯 100ms 亮/400ms 灭 */
}

void status_led_set_steering(bool active)
{
    s_steering_active = active;
    status_led_refresh();
}

void status_led_set_zigbee_ready(void)
{
    s_zigbee_ready = true;
    status_led_refresh();
}

void status_led_start_ready_poll(void)
{
    if (s_poll_timer_started) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = &ready_poll_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "status_poll",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_ready_poll_timer) == ESP_OK) {
        esp_timer_start_periodic(s_ready_poll_timer, STATUS_READY_POLL_MS * 1000);
        s_poll_timer_started = true;
    }
}

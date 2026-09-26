/*
   SPDX-License-Identifier: Apache-2.0

   Gateway 系统状态 LED 通知接口。
   bridge_manager 在 Matter 配网完成时调用，app_main 实现具体 LED 逻辑。
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 通知：Matter 已配网/会话就绪，状态 LED 应常亮
 *
 * 由 bridge_manager_event_cb 在 kCommissioningComplete 时调用；
 * 也可由 app_main 在启动时检测到"已有 fabric"后调用（重启后无需重新配网）。
 */
void app_status_notify_ready(void);

#ifdef __cplusplus
}
#endif
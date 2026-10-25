/*
   SPDX-License-Identifier: Apache-2.0

   外部 RTC(RX8025T) 定时读数诊断（测试用，可整块裁剪）。
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 RTC 定时读数诊断任务（每 10s 打印一次）
 *
 * 由 RTC_TIME_DIAG_ENABLE 控制，关闭时本函数为空实现。
 */
void rtc_time_diag_start(void);

#ifdef __cplusplus
}
#endif

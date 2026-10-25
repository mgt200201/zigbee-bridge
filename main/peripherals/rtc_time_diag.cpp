/*
   SPDX-License-Identifier: Apache-2.0

   外部 RTC(RX8025T) 定时读数诊断实现。

   每 10s 从 RTC 读取时间并打印，用于验证走时/校时是否正确。
   不需要时把 RTC_TIME_DIAG_ENABLE 改成 0 即可整块关闭。
*/

#include "rtc_time_diag.h"

#include <time.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "rtc_rx8025t.h"

static const char *TAG = "rtc_diag";

#define RTC_TIME_DIAG_ENABLE        1
#if RTC_TIME_DIAG_ENABLE

#define RTC_TIME_DIAG_PERIOD_MS     10000
#define RTC_TIME_DIAG_TASK_STACK    3072

/* 以编译年份为基准判断 RTC 是否"看起来合理"：RX8025T 从未被校时时寄存器为上电随机值，
 * 实测会落在固定区间之内（读到过 2088），所以不能只用 2020~2099 这种宽区间。 */
#define RTC_BUILD_YEAR          ((__DATE__[7]-'0')*1000 + (__DATE__[8]-'0')*100 \
                                + (__DATE__[9]-'0')*10 + (__DATE__[10]-'0'))
#define RTC_TIME_VALID_YEAR_MIN     (RTC_BUILD_YEAR - 1)
#define RTC_TIME_VALID_YEAR_MAX     (RTC_BUILD_YEAR + 20)

static void rtc_time_diag_task(void *arg)
{
    (void)arg;
    for (;;) {
        struct tm t = {0};
        if (rtc_rx8025t_get_time(&t) == ESP_OK) {
            int year = t.tm_year + 1900;
            if (year < RTC_TIME_VALID_YEAR_MIN || year > RTC_TIME_VALID_YEAR_MAX) {
                /* 联网 SNTP 校时成功前，RTC 读数无意义，明确标注避免误判 */
                ESP_LOGW(TAG, "RTC not calibrated yet (raw %04d-%02d-%02d %02d:%02d:%02d)",
                         year, t.tm_mon + 1, t.tm_mday,
                         t.tm_hour, t.tm_min, t.tm_sec);
            } else {
                ESP_LOGI(TAG, "RTC now: %04d-%02d-%02d %02d:%02d:%02d (wday=%d)",
                         year, t.tm_mon + 1, t.tm_mday,
                         t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday);
            }
        } else {
            ESP_LOGW(TAG, "RTC read failed");
        }
        vTaskDelay(pdMS_TO_TICKS(RTC_TIME_DIAG_PERIOD_MS));
    }
}

#endif /* RTC_TIME_DIAG_ENABLE */

void rtc_time_diag_start(void)
{
#if RTC_TIME_DIAG_ENABLE
    if (xTaskCreate(rtc_time_diag_task, "rtc_time_diag", RTC_TIME_DIAG_TASK_STACK,
                    NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to create rtc_time_diag task");
    }
#endif
}

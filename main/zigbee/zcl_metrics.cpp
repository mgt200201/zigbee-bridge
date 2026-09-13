/*
   This example code is in the Public Domain (CC0 licensed), at your option.

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zcl_metrics.cpp
 *
 * @brief 轻量级运行时指标实现
 */

#include "zcl_metrics.h"
#include <esp_log.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static const char *TAG = "zcl_metrics";

/** 全局 metrics 实例 */
static zcl_metrics_t s_metrics = { 0 };

zcl_metrics_t *zcl_metrics_get(void)
{
    return &s_metrics;
}

void zcl_metrics_record_dispatch(bool handled)
{
    s_metrics.total_dispatched++;
    if (handled) {
        s_metrics.total_handled++;
    } else {
        s_metrics.total_unhandled++;
    }
}

void zcl_metrics_record_unhandled(uint16_t cluster_id)
{
    /* 环形缓冲：写入最新条目，覆盖最旧的 */
    uint8_t idx = s_metrics.unhandled_ring_idx;
    s_metrics.unhandled_ring[idx] = cluster_id;
    s_metrics.unhandled_ring_idx = (idx + 1) % METRICS_UNHANDLED_RING_SIZE;
}

void zcl_metrics_record_discovery(bool success)
{
    if (success) {
        s_metrics.discovery_ok++;
    } else {
        s_metrics.discovery_fail++;
    }
}

void zcl_metrics_record_discovery_retry(void)
{
    s_metrics.discovery_retry++;
}

static const char *severity_label(zcl_error_severity_t s)
{
    switch (s) {
    case ZCL_ERR_SEVERITY_TRANSIENT: return "TRANSIENT";
    case ZCL_ERR_SEVERITY_PERMANENT: return "PERMANENT";
    case ZCL_ERR_SEVERITY_DEGRADED:  return "DEGRADED";
    default:                         return "UNKNOWN";
    }
}

void zcl_metrics_record_error(zcl_error_severity_t severity, const char *tag,
                               const char *format, ...)
{
    /* 更新分类计数器 */
    switch (severity) {
    case ZCL_ERR_SEVERITY_TRANSIENT:
        s_metrics.err_transient++;
        break;
    case ZCL_ERR_SEVERITY_PERMANENT:
        s_metrics.err_permanent++;
        /* PERMANENT 级别同时记入 ring buffer */
        zcl_metrics_record_unhandled(0xFFFF);
        break;
    case ZCL_ERR_SEVERITY_DEGRADED:
        s_metrics.err_degraded++;
        break;
    }

    /* 格式化并打印日志 */
    char buf[160];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    /* 统一日志格式：分类 + 原始消息 */
    ESP_LOGE(tag, "[%s] %s", severity_label(severity), buf);
}

void zcl_metrics_dump(void)
{
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║         ZCL Runtime Metrics                  ║");
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ Dispatch:  total=%-5u handled=%-5u miss=%-5u     ║",
             (unsigned)s_metrics.total_dispatched,
             (unsigned)s_metrics.total_handled,
             (unsigned)s_metrics.total_unhandled);
    ESP_LOGI(TAG, "║ Errors:    T=%-5u P=%-5u D=%-5u               ║",
             (unsigned)s_metrics.err_transient,
             (unsigned)s_metrics.err_permanent,
             (unsigned)s_metrics.err_degraded);
    ESP_LOGI(TAG, "║ Discovery: ok=%-5u fail=%-5u retry=%-5u         ║",
             (unsigned)s_metrics.discovery_ok,
             (unsigned)s_metrics.discovery_fail,
             (unsigned)s_metrics.discovery_retry);
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ Unhandled Clusters (ring buffer):           ║");
    for (int i = 0; i < METRICS_UNHANDLED_RING_SIZE; i++) {
        int idx = (s_metrics.unhandled_ring_idx + i) % METRICS_UNHANDLED_RING_SIZE;
        if (s_metrics.unhandled_ring[idx] != 0) {
            ESP_LOGI(TAG, "║   [%d] 0x%04x                                    ║",
                     i, s_metrics.unhandled_ring[idx]);
        }
    }
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════╝");
}

void zcl_metrics_reset(void)
{
    memset(&s_metrics, 0, sizeof(s_metrics));
    ESP_LOGI(TAG, "Metrics reset");
}
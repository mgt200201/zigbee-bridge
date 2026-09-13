/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zcl_metrics.h
 *
 * @brief 轻量级运行时指标（运维可观测性）
 *
 * 大厂标准实践：生产级网关必须有可观测性指标，用于：
 *   - 发现未处理的 cluster（配置遗漏或 bug）
 *   - 统计设备发现成功率
 *   - 监控消息处理吞吐量
 *
 * 当前实现使用内存计数器 + 环形缓冲区记录未处理 cluster。
 * 后续可扩展为：
 *   - 通过 NVS 持久化（重启后也可查看历史）
 *   - 通过 Matter Diagnostic Cluster 上报
 *   - 集成 ESP Insights 远程监控
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ==================== 错误严重级别 ==================== */

/**
 * @brief 错误严重级别三态分类
 *
 * 大厂生产级标准：任何运行时错误必须分类，禁止全部归为"ERROR"。
 *
 * 分类策略：
 *   TRANSIENT  → 临时故障，自动重试可恢复（如：无线丢包、时序竞争）
 *   PERMANENT  → 固件 bug 或协议不兼容，重试也无效（如：未知 cluster、数据格式错误）
 *   DEGRADED   → 功能降级，系统可继续运行但部分功能不可用（如：某个传感器读数异常）
 */
typedef enum {
    ZCL_ERR_SEVERITY_TRANSIENT = 0,    /**< 临时故障，可重试 */
    ZCL_ERR_SEVERITY_PERMANENT,        /**< 固件 bug / 协议不兼容 */
    ZCL_ERR_SEVERITY_DEGRADED,         /**< 功能降级，不崩溃 */
} zcl_error_severity_t;

/** ==================== 指标结构 ==================== */

/** 未处理 cluster 环形缓冲区大小 */
#define METRICS_UNHANDLED_RING_SIZE  8

typedef struct {
    /* ── 分发统计 ── */
    uint32_t total_dispatched;          /**< ZCL 消息总分发次数 */
    uint32_t total_handled;             /**< 成功找到 handler 的次数 */
    uint32_t total_unhandled;           /**< 无对应 handler 的次数 */

    /* ── 错误分类统计 ── */
    uint32_t err_transient;             /**< 临时故障计数 */
    uint32_t err_permanent;             /**< 永久故障计数 */
    uint32_t err_degraded;              /**< 降级运行计数 */

    /* ── 未处理 cluster 环形缓冲（便于排查未注册的 cluster） ── */
    uint16_t unhandled_ring[METRICS_UNHANDLED_RING_SIZE];
    uint8_t  unhandled_ring_idx;

    /* ── 设备发现 ── */
    uint32_t discovery_ok;              /**< 设备发现成功次数 */
    uint32_t discovery_fail;            /**< 设备发现失败次数 */
    uint32_t discovery_retry;           /**< 发现重试次数 */

} zcl_metrics_t;

/** ==================== API ==================== */

/** 获取全局 metrics 实例指针 */
zcl_metrics_t *zcl_metrics_get(void);

/** 记录一次分发 */
void zcl_metrics_record_dispatch(bool handled);

/** 记录未处理的 cluster_id */
void zcl_metrics_record_unhandled(uint16_t cluster_id);

/** 记录设备发现结果 */
void zcl_metrics_record_discovery(bool success);

/** 记录发现重试 */
void zcl_metrics_record_discovery_retry(void);

/**
 * @brief 记录一次分类错误
 *
 * 大厂标准：所有错误必须分类，不得使用裸 ESP_LOGE。
 * 本函数自动：
 *   - 增加对应严重级别的计数器
 *   - 如果是 PERMANENT 类型，同时记入未处理 ring buffer
 *   - 打印带错误上下文的日志
 *
 * @param severity  错误严重级别
 * @param tag       LOG 标签（通常用模块 TAG）
 * @param format    日志格式（printf 风格）
 * @param ...       格式化参数
 */
void zcl_metrics_record_error(zcl_error_severity_t severity, const char *tag,
                               const char *format, ...) __attribute__((format(printf, 3, 4)));

/** 打印当前指标到日志（可定时调用或通过控制台触发） */
void zcl_metrics_dump(void);

/** 重置所有指标 */
void zcl_metrics_reset(void);

#ifdef __cplusplus
}
#endif
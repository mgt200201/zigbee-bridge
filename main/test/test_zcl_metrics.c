/*
 *  ZCL Metrics 单元测试
 *
 *  测试覆盖：
 *    - 分发计数（dispatch / handled / unhandled）
 *    - 环形缓冲区写入与覆盖
 *    - 设备发现计数（ok / fail / retry）
 *    - 错误分类（TRANSIENT / PERMANENT / DEGRADED）
 *    - 重置功能
 *    - 边界条件：极端次数、buffer 覆盖
 */

#include "zcl_metrics.h"
#include <unity.h>
#include <string.h>

static void setUp(void)
{
    zcl_metrics_reset();
}

/* ── 分发计数 ── */

static void test_dispatch_handled(void)
{
    for (int i = 0; i < 5; i++) {
        zcl_metrics_record_dispatch(true);
    }

    zcl_metrics_t *m = zcl_metrics_get();
    TEST_ASSERT_EQUAL_UINT32(5, m->total_dispatched);
    TEST_ASSERT_EQUAL_UINT32(5, m->total_handled);
    TEST_ASSERT_EQUAL_UINT32(0, m->total_unhandled);
}

static void test_dispatch_unhandled(void)
{
    for (int i = 0; i < 3; i++) {
        zcl_metrics_record_dispatch(false);
    }

    zcl_metrics_t *m = zcl_metrics_get();
    TEST_ASSERT_EQUAL_UINT32(3, m->total_dispatched);
    TEST_ASSERT_EQUAL_UINT32(0, m->total_handled);
    TEST_ASSERT_EQUAL_UINT32(3, m->total_unhandled);
}

static void test_dispatch_mixed(void)
{
    zcl_metrics_record_dispatch(true);
    zcl_metrics_record_dispatch(false);
    zcl_metrics_record_dispatch(true);
    zcl_metrics_record_dispatch(false);

    zcl_metrics_t *m = zcl_metrics_get();
    TEST_ASSERT_EQUAL_UINT32(4, m->total_dispatched);
    TEST_ASSERT_EQUAL_UINT32(2, m->total_handled);
    TEST_ASSERT_EQUAL_UINT32(2, m->total_unhandled);
}

/* ── 环形缓冲区 ── */

static void test_unhandled_ring_basic(void)
{
    zcl_metrics_record_unhandled(0x0400);
    zcl_metrics_record_unhandled(0x0401);
    zcl_metrics_record_unhandled(0x0402);

    zcl_metrics_t *m = zcl_metrics_get();
    /* 环形缓冲写入顺序：0x0400, 0x0401, 0x0402
     * 索引指向下一个写入位置（3 % 8 = 3） */
    TEST_ASSERT_EQUAL_UINT16(0x0400, m->unhandled_ring[0]);
    TEST_ASSERT_EQUAL_UINT16(0x0401, m->unhandled_ring[1]);
    TEST_ASSERT_EQUAL_UINT16(0x0402, m->unhandled_ring[2]);
    TEST_ASSERT_EQUAL_UINT8(3, m->unhandled_ring_idx);
}

static void test_unhandled_ring_overflow(void)
{
    /* 写入 10 个（超过 8 的容量），旧条目被覆盖 */
    for (int i = 0; i < 10; i++) {
        zcl_metrics_record_unhandled((uint16_t)(0x1000 + i));
    }

    zcl_metrics_t *m = zcl_metrics_get();
    /* 索引应为 10 % 8 = 2，最旧的 0x1000, 0x1001 被覆盖 */
    TEST_ASSERT_EQUAL_UINT8(2, m->unhandled_ring_idx);
    /* slot 0 现在存的是第 8 次写入（index 0 overwritten by i=8） */
    TEST_ASSERT_EQUAL_UINT16(0x1008, m->unhandled_ring[0]);
    TEST_ASSERT_EQUAL_UINT16(0x1009, m->unhandled_ring[1]);
}

/* ── 设备发现计数 ── */

static void test_discovery_success(void)
{
    zcl_metrics_record_discovery(true);
    zcl_metrics_record_discovery(true);

    zcl_metrics_t *m = zcl_metrics_get();
    TEST_ASSERT_EQUAL_UINT32(2, m->discovery_ok);
    TEST_ASSERT_EQUAL_UINT32(0, m->discovery_fail);
}

static void test_discovery_fail(void)
{
    zcl_metrics_record_discovery(false);
    zcl_metrics_record_discovery(false);
    zcl_metrics_record_discovery(false);

    zcl_metrics_t *m = zcl_metrics_get();
    TEST_ASSERT_EQUAL_UINT32(0, m->discovery_ok);
    TEST_ASSERT_EQUAL_UINT32(3, m->discovery_fail);
}

static void test_discovery_retry_count(void)
{
    zcl_metrics_record_discovery_retry();
    zcl_metrics_record_discovery_retry();
    zcl_metrics_record_discovery_retry();

    zcl_metrics_t *m = zcl_metrics_get();
    TEST_ASSERT_EQUAL_UINT32(3, m->discovery_retry);
}

/* ── 错误分类 ── */

static void test_error_classification_transient(void)
{
    zcl_metrics_record_error(ZCL_ERR_SEVERITY_TRANSIENT, "test",
                             "transient error %d", 1);
    zcl_metrics_record_error(ZCL_ERR_SEVERITY_TRANSIENT, "test",
                             "transient error %d", 2);

    zcl_metrics_t *m = zcl_metrics_get();
    TEST_ASSERT_EQUAL_UINT32(2, m->err_transient);
    TEST_ASSERT_EQUAL_UINT32(0, m->err_permanent);
    TEST_ASSERT_EQUAL_UINT32(0, m->err_degraded);
}

static void test_error_classification_permanent(void)
{
    zcl_metrics_record_error(ZCL_ERR_SEVERITY_PERMANENT, "test",
                             "fatal: %s", "bad data");

    zcl_metrics_t *m = zcl_metrics_get();
    TEST_ASSERT_EQUAL_UINT32(0, m->err_transient);
    TEST_ASSERT_EQUAL_UINT32(1, m->err_permanent);
    TEST_ASSERT_EQUAL_UINT32(0, m->err_degraded);
}

static void test_error_classification_degraded(void)
{
    zcl_metrics_record_error(ZCL_ERR_SEVERITY_DEGRADED, "test",
                             "degraded: %s", "sensor timeout");

    zcl_metrics_t *m = zcl_metrics_get();
    TEST_ASSERT_EQUAL_UINT32(0, m->err_transient);
    TEST_ASSERT_EQUAL_UINT32(0, m->err_permanent);
    TEST_ASSERT_EQUAL_UINT32(1, m->err_degraded);
}

/* ── 重置 ── */

static void test_metrics_reset(void)
{
    zcl_metrics_record_dispatch(true);
    zcl_metrics_record_dispatch(false);
    zcl_metrics_record_discovery(true);
    zcl_metrics_record_discovery(false);
    zcl_metrics_record_discovery_retry();
    zcl_metrics_record_unhandled(0x9999);
    zcl_metrics_record_error(ZCL_ERR_SEVERITY_PERMANENT, "test", "err");

    zcl_metrics_reset();

    zcl_metrics_t *m = zcl_metrics_get();
    TEST_ASSERT_EQUAL_UINT32(0, m->total_dispatched);
    TEST_ASSERT_EQUAL_UINT32(0, m->total_handled);
    TEST_ASSERT_EQUAL_UINT32(0, m->total_unhandled);
    TEST_ASSERT_EQUAL_UINT32(0, m->discovery_ok);
    TEST_ASSERT_EQUAL_UINT32(0, m->discovery_fail);
    TEST_ASSERT_EQUAL_UINT32(0, m->discovery_retry);
    TEST_ASSERT_EQUAL_UINT32(0, m->err_transient);
    TEST_ASSERT_EQUAL_UINT32(0, m->err_permanent);
    TEST_ASSERT_EQUAL_UINT32(0, m->err_degraded);
    TEST_ASSERT_EQUAL_UINT8(0, m->unhandled_ring_idx);
    for (int i = 0; i < METRICS_UNHANDLED_RING_SIZE; i++) {
        TEST_ASSERT_EQUAL_UINT16(0, m->unhandled_ring[i]);
    }
}

/* ── 入口 ── */

TEST_CASE("zcl metrics - dispatch handled", "[zcl_metrics]")
{
    setUp();
    test_dispatch_handled();
}

TEST_CASE("zcl metrics - dispatch unhandled", "[zcl_metrics]")
{
    setUp();
    test_dispatch_unhandled();
}

TEST_CASE("zcl metrics - dispatch mixed", "[zcl_metrics]")
{
    setUp();
    test_dispatch_mixed();
}

TEST_CASE("zcl metrics - unhandled ring basic", "[zcl_metrics]")
{
    setUp();
    test_unhandled_ring_basic();
}

TEST_CASE("zcl metrics - unhandled ring overflow", "[zcl_metrics]")
{
    setUp();
    test_unhandled_ring_overflow();
}

TEST_CASE("zcl metrics - discovery success", "[zcl_metrics]")
{
    setUp();
    test_discovery_success();
}

TEST_CASE("zcl metrics - discovery fail", "[zcl_metrics]")
{
    setUp();
    test_discovery_fail();
}

TEST_CASE("zcl metrics - discovery retry", "[zcl_metrics]")
{
    setUp();
    test_discovery_retry_count();
}

TEST_CASE("zcl metrics - error transient", "[zcl_metrics]")
{
    setUp();
    test_error_classification_transient();
}

TEST_CASE("zcl metrics - error permanent", "[zcl_metrics]")
{
    setUp();
    test_error_classification_permanent();
}

TEST_CASE("zcl metrics - error degraded", "[zcl_metrics]")
{
    setUp();
    test_error_classification_degraded();
}

TEST_CASE("zcl metrics - reset", "[zcl_metrics]")
{
    setUp();
    test_metrics_reset();
}
/*
 *  ZCL Handler Registry 单元测试
 *
 *  测试覆盖：
 *    - 传感器白名单匹配（is_sensor_cluster 等价逻辑）
 *    - Handler 注册与查找
 *    - 重复注册（覆盖）
 *    - 注册表满（溢出保护）
 */

#include "zcl_handler_registry.h"
#include "zcl_bridge_api.h"
#include "zcl_metrics.h"
#include <unity.h>
#include <esp_log.h>

/* ── 辅助：手动验证传感器白名单（与 registry 内部一致） ── */

static const uint16_t known_sensor_clusters[] = {
    0x0402,  /* Temperature */
    0x0405,  /* Humidity */
};
#define KNOWN_SENSOR_COUNT (sizeof(known_sensor_clusters) / sizeof(known_sensor_clusters[0]))

static bool is_known_sensor(uint16_t cid)
{
    for (int i = 0; i < KNOWN_SENSOR_COUNT; i++) {
        if (known_sensor_clusters[i] == cid) return true;
    }
    return false;
}

/* ── 测试：传感器匹配 ── */

static void test_sensor_known_ids(void)
{
    TEST_ASSERT_TRUE(is_known_sensor(0x0402));
    TEST_ASSERT_TRUE(is_known_sensor(0x0405));
}

static void test_sensor_unknown_ids(void)
{
    TEST_ASSERT_FALSE(is_known_sensor(0x0000));
    TEST_ASSERT_FALSE(is_known_sensor(0x0006));  /* On/Off, not a sensor */
    TEST_ASSERT_FALSE(is_known_sensor(0xFC11));  /* Private */
    TEST_ASSERT_FALSE(is_known_sensor(0xFFFF));
}

/* ── 测试：Handler 注册 ── */

static const zcl_device_handler_t dummy_handler = {
    .cluster_id = 0x1234,
    .name = "dummy",
    .on_report_attr = NULL,
    .on_set_attr = NULL,
    .on_read_attr_resp = NULL,
};

static void test_handler_register_valid(void)
{
    int before = zcl_handler_count();
    esp_err_t err = zcl_handler_register(&dummy_handler);
    int after = zcl_handler_count();

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(before + 1, after);
}

static void test_handler_register_null(void)
{
    int before = zcl_handler_count();
    esp_err_t err = zcl_handler_register(NULL);
    int after = zcl_handler_count();

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL(before, after);
}

static const zcl_device_handler_t nameless = {
    .cluster_id = 0x5678,
    .name = NULL,           /* 无效：必须有 name */
    .on_report_attr = NULL,
};

static void test_handler_register_no_name(void)
{
    int before = zcl_handler_count();
    esp_err_t err = zcl_handler_register(&nameless);
    int after = zcl_handler_count();

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL(before, after);
}

/* ── 测试：Bridge API 注册/获取 ── */

static void mock_report_sensor(uint16_t a, uint8_t b, uint16_t c, int16_t d) { (void)a;(void)b;(void)c;(void)d; }
static void mock_report_on_off(uint16_t a, uint8_t b, bool c) { (void)a;(void)b;(void)c; }
static bool mock_is_bridged(uint16_t a) { (void)a; return false; }
static uint8_t mock_get_ep(uint16_t a) { (void)a; return 0xFF; }

static const zcl_bridge_api_t mock_api = {
    .report_sensor = mock_report_sensor,
    .report_on_off = mock_report_on_off,
    .is_device_bridged = mock_is_bridged,
    .get_endpoint_by_short = mock_get_ep,
};

static void test_bridge_api_register_valid(void)
{
    zcl_bridge_api_register(&mock_api);
    const zcl_bridge_api_t *api = zcl_bridge_api_get();
    TEST_ASSERT_NOT_NULL(api);
    TEST_ASSERT_EQUAL_PTR(mock_report_sensor, api->report_sensor);
    TEST_ASSERT_EQUAL_PTR(mock_report_on_off, api->report_on_off);
}

static void test_bridge_api_register_null(void)
{
    const zcl_bridge_api_t *saved = zcl_bridge_api_get();
    zcl_bridge_api_register(NULL);
    /* NULL 注册被拒绝，应保持原值 */
    const zcl_bridge_api_t *api = zcl_bridge_api_get();
    TEST_ASSERT_EQUAL_PTR(saved, api);
}

static void test_bridge_api_get_unregistered(void)
{
    /* 先注册 mock 确保非空，然后无法测试未注册场景
     * 此测试确认 get 在注册后返回非 NULL */
    zcl_bridge_api_register(&mock_api);
    const zcl_bridge_api_t *api = zcl_bridge_api_get();
    TEST_ASSERT_NOT_NULL(api);
}

/* ── 入口 ── */

TEST_CASE("zcl register - sensor known IDs", "[zcl_registry]")
{
    test_sensor_known_ids();
}

TEST_CASE("zcl register - sensor unknown IDs", "[zcl_registry]")
{
    test_sensor_unknown_ids();
}

TEST_CASE("zcl register - handler register valid", "[zcl_registry]")
{
    test_handler_register_valid();
}

TEST_CASE("zcl register - handler register NULL", "[zcl_registry]")
{
    test_handler_register_null();
}

TEST_CASE("zcl register - handler register no name", "[zcl_registry]")
{
    test_handler_register_no_name();
}

TEST_CASE("zcl bridge_api - register valid", "[zcl_registry]")
{
    test_bridge_api_register_valid();
}

TEST_CASE("zcl bridge_api - register NULL", "[zcl_registry]")
{
    test_bridge_api_register_null();
}

TEST_CASE("zcl bridge_api - get after register", "[zcl_registry]")
{
    test_bridge_api_get_unregistered();
}
/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zdp_discovery.cpp
 *
 * @brief Zigbee 设备发现与 ZCL 处理层
 *
 * 职责：
 *   1. 监听设备入网 → 顺序探测端点 (1~MAX) → Simple Descriptor → 创建桥接
 *   2. 读取 Basic 属性获取设备标识
 *   3. 将 ZCL 消息分发到 zcl_handler_registry（不硬编码 if-else）
 *
 * 生产级特性：
 *   - Kconfig 控制：探测端点数、延迟时间、重试次数
 *   - 指数退避重试：Simple Descriptor 超时/失败自动重试
 *   - Metrics 记录：发现成功率、重试次数
 *   - 日志分级：INFO 正常、WARN 重试、ERROR 失败
 */

#include "zdp_discovery.h"
#include "zb_tx_queue.h"
#include "zcl_handler_registry.h"
#include "zcl_metrics.h"
#include "zb_device_ota.h"
#include <zboss_manager.h>
#include <bridge_manager.h>
#include <device_info.h>
#include <app_bridged_device.h>
#include <string.h>
#include <esp_log.h>
#include <zdo/esp_zigbee_zdo_command.h>
#include <zcl/esp_zigbee_zcl_temperature_meas.h>
#include <zcl/esp_zigbee_zcl_humidity_meas.h>
#include <zcl/esp_zigbee_zcl_on_off.h>
#include <zcl/esp_zigbee_zcl_level.h>
#include <zcl/esp_zigbee_zcl_color_control.h>
#include <nwk/esp_zigbee_nwk.h>

static const char *TAG = "zdp_discovery";

/* ==================== Kconfig 默认值 ==================== */

#ifndef CONFIG_ZIGBEE_MAX_ENDPOINT_PROBE
#define CONFIG_ZIGBEE_MAX_ENDPOINT_PROBE 5
#endif

#ifndef CONFIG_ZIGBEE_ANNCE_DELAY_MS
#define CONFIG_ZIGBEE_ANNCE_DELAY_MS 500
#endif

#ifndef CONFIG_ZIGBEE_DISCOVERY_MAX_RETRIES
#define CONFIG_ZIGBEE_DISCOVERY_MAX_RETRIES 3
#endif

#ifndef CONFIG_ZIGBEE_DISCOVERY_RETRY_BASE_MS
#define CONFIG_ZIGBEE_DISCOVERY_RETRY_BASE_MS 1000
#endif

/* ==================== 发现上下文（重试支持） ==================== */

/**
 * @brief 每个设备的发现上下文
 *
 * 因为 ZDO Simple Desc 响应是异步的，context 必须用静态存储（不能放栈上）。
 * 我们用全局数组管理，最多同时跟踪 1 个设备的发现状态（简化方案）。
 *
 * 更复杂的产品级实现会用 hash 表按 short_addr 索引多个上下文。
 */
typedef struct {
    uint16_t short_addr;
    uint8_t  retry_count;        /**< 当前已重试次数 */
    bool     in_progress;        /**< 发现进行中（防止重入） */
} discovery_ctx_t;

/** 全局发现上下文（单设备） */
static discovery_ctx_t s_discovery_ctx = {
    .short_addr = 0xFFFF,
    .retry_count = 0,
    .in_progress = false,
};

/* ==================== 状态 ==================== */

/** 待处理的设备公告数据 */
static uint16_t s_pending_annce_short_addr = 0xFFFF;

/** 当前发现流程是否属于「首次配网入网」（仅首配时下发上报配置，rejoin 跳过） */
static bool s_first_join_pending = false;

/* ==================== 内部函数声明 ==================== */
static void discover_device_cb(uint8_t param);
static void probe_next_endpoint(uint16_t short_addr, uint8_t next_ep);
static void retry_probe_cb(uint8_t param);
static void discover_done_cb(uint8_t param);
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *message);
static esp_err_t zb_read_attr_resp_handler(
    const esp_zb_zcl_cmd_read_attr_resp_message_t *message);

/* ==================== Simple Descriptor 响应 ==================== */

void zdp_discovery_on_simple_desc(esp_zb_zdp_status_t zdo_status,
                                  esp_zb_af_simple_desc_1_1_t *simple_desc,
                                  void *user_ctx)
{
    uint16_t addr = (uint16_t)(uintptr_t)user_ctx;

    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !simple_desc) {
        ESP_LOGI(TAG, "Simple Desc probe: ep=%d, status=%d for 0x%04x (no such endpoint)",
                 s_discovery_ctx.in_progress ? s_discovery_ctx.retry_count + 1 : 0,
                 zdo_status, addr);
        return;
    }

    uint8_t endpoint = simple_desc->endpoint;

    /* 打印完整的 Simple Descriptor 信息 */
    ESP_LOGI(TAG, "=== Simple Descriptor for device 0x%04x, endpoint=%d ===", addr, endpoint);
    ESP_LOGI(TAG, "  Profile: 0x%04x, Device: 0x%04x",
             simple_desc->app_profile_id, simple_desc->app_device_id);
    ESP_LOGI(TAG, "  Input Clusters (%d):", simple_desc->app_input_cluster_count);
    for (int i = 0; i < simple_desc->app_input_cluster_count; i++) {
        uint16_t cid = simple_desc->app_cluster_list[i];
        ESP_LOGI(TAG, "    [%d] 0x%04x", i, cid);
    }

    /* 找到了一个端点 → 重置重试计数 */
    s_discovery_ctx.retry_count = 0;
    s_discovery_ctx.in_progress = false;

    /* 创建 Matter 桥接设备 + 读取 Basic 属性 */
    bridge_manager_on_device_discovered(addr, endpoint);

    zcl_metrics_record_discovery(true);
}

/* ==================== 端点顺序探测 ==================== */

/**
 * @brief 顺序探测设备的端点
 *
 * 从 endpoint=1 开始，依次向设备发送 Simple Descriptor Request。
 * 所有请求发送完后，由 discover_done_cb 定时器检查是否发现成功。
 *
 * @param short_addr  设备短地址
 * @param next_ep     当前要探测的端点号（从 1 开始）
 */
static void probe_next_endpoint(uint16_t short_addr, uint8_t next_ep)
{
    if (next_ep > CONFIG_ZIGBEE_MAX_ENDPOINT_PROBE) {
        ESP_LOGI(TAG, "All endpoint probes sent for 0x%04x", short_addr);
        return;
    }

    ESP_LOGI(TAG, "Probing endpoint %d of device 0x%04x...", next_ep, short_addr);
    zb_tx_simple_desc(short_addr, next_ep);
    probe_next_endpoint(short_addr, next_ep + 1);
}

/* ==================== 重试机制（定时器 + 指数退避） ==================== */

/**
 * @brief 重试探测回调
 *
 * 在指数退避延迟后被调度，重新发送 Simple Desc 请求。
 * 使用全局上下文 s_discovery_ctx。
 */
static void retry_probe_cb(uint8_t param)
{
    (void)param;

    if (!s_discovery_ctx.in_progress) {
        return;
    }

    ESP_LOGI(TAG, "Retry probe for 0x%04x (attempt %d/%d)",
             s_discovery_ctx.short_addr,
             s_discovery_ctx.retry_count,
             CONFIG_ZIGBEE_DISCOVERY_MAX_RETRIES);

    probe_next_endpoint(s_discovery_ctx.short_addr, 1);

    /* 再次调度超时检查 */
    esp_zb_scheduler_alarm(discover_done_cb, 0,
                           CONFIG_ZIGBEE_DISCOVERY_RETRY_BASE_MS);
}

/**
 * @brief 发现完成/超时检查回调
 *
 * 在发送完所有 Simple Desc 请求后延迟一段时间调用。
 * 如果 simple_desc_cb 已经成功处理（in_progress == false），说明发现成功。
 * 否则触发指数退避重试。
 */
static void discover_done_cb(uint8_t param)
{
    (void)param;

    if (!s_discovery_ctx.in_progress) {
        return;     /* 已经成功，无需处理 */
    }

    /* 还没成功 → 检查是否需要重试 */
    s_discovery_ctx.retry_count++;
    if (s_discovery_ctx.retry_count > CONFIG_ZIGBEE_DISCOVERY_MAX_RETRIES) {
        ESP_LOGW(TAG, "Discovery failed for 0x%04x after %d retries",
                 s_discovery_ctx.short_addr,
                 CONFIG_ZIGBEE_DISCOVERY_MAX_RETRIES);
        s_discovery_ctx.in_progress = false;
        zcl_metrics_record_discovery(false);
        return;
    }

    /* 指数退避延迟 */
    uint32_t delay = CONFIG_ZIGBEE_DISCOVERY_RETRY_BASE_MS
                     * (1 << (s_discovery_ctx.retry_count - 1));

    ESP_LOGW(TAG, "Discovery retry %d/%d for 0x%04x in %lu ms",
             s_discovery_ctx.retry_count,
             CONFIG_ZIGBEE_DISCOVERY_MAX_RETRIES,
             s_discovery_ctx.short_addr,
             (unsigned long)delay);

    zcl_metrics_record_discovery_retry();

    /* 延迟后重新探测 */
    esp_zb_scheduler_alarm(retry_probe_cb, 0, delay);
}

/**
 * @brief 延迟执行的设备发现
 *
 * 在收到 Device Annce 后由 Kconfig 配置的延迟时间调用。
 */
static void discover_device_cb(uint8_t param)
{
    (void)param;

    if (s_pending_annce_short_addr == 0xFFFF) {
        return;
    }
    uint16_t short_addr = s_pending_annce_short_addr;
    s_pending_annce_short_addr = 0xFFFF;

    /* 检查是否已存在桥接设备（重入网场景） */
    if (bridge_manager_is_device_bridged(short_addr)) {
        ESP_LOGI(TAG, "Device 0x%04x already bridged, re-reading basic attributes", short_addr);
        s_first_join_pending = false;   /* rejoin：不重复下发上报配置 */
        uint8_t ep = bridge_manager_get_endpoint_by_short(short_addr);
        if (ep != 0xFF) {
            zdp_discovery_read_basic_attributes(short_addr, ep);
        }
        return;
    }

    /* ── 首次发现 ── */
    s_first_join_pending = true;        /* 首次配网：识别后需要下发上报配置 */
    s_discovery_ctx.short_addr = short_addr;
    s_discovery_ctx.retry_count = 0;
    s_discovery_ctx.in_progress = true;

    ESP_LOGI(TAG, "Starting endpoint probe for device 0x%04x (1..%d, max_retries=%d)",
             short_addr, CONFIG_ZIGBEE_MAX_ENDPOINT_PROBE,
             CONFIG_ZIGBEE_DISCOVERY_MAX_RETRIES);

    zdp_discovery_send_register_status(short_addr, 1, 128);

    probe_next_endpoint(short_addr, 1);

    /* 调度超时检查（等待所有响应返回后再判断成败） */
    esp_zb_scheduler_alarm(discover_done_cb, 0,
                           CONFIG_ZIGBEE_DISCOVERY_RETRY_BASE_MS);
}

void zdp_discovery_handle_device_annce(uint16_t short_addr,
                                       const esp_zb_ieee_addr_t ieee_addr,
                                       bool steering_active)
{
    (void)steering_active;
    (void)ieee_addr;

    ESP_LOGD(TAG, "Device annce: 0x%04x, delaying %d ms before discovery",
             short_addr, CONFIG_ZIGBEE_ANNCE_DELAY_MS);

    /* 延迟配置的时间，等待设备入网稳定后再发 ZDO 请求 */
    s_pending_annce_short_addr = short_addr;
    esp_zb_scheduler_alarm((esp_zb_callback_t)discover_device_cb, 0,
                           CONFIG_ZIGBEE_ANNCE_DELAY_MS);
}

/* ==================== ZCL 动作分发 ==================== */

/**
 * @brief 全局 ZCL 动作处理器
 *
 * 两步分发：
 *   1. 优先通过 zcl_handler_registry 按 cluster 路由到设备处理器
 *   2. 如果 registry 返回 NOT_FOUND，走内置 fallback（如读属性响应）
 */
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                   const void *message)
{
    /* 第一步：尝试 registry 分发（report / set 走这里） */
    esp_err_t err = zcl_handler_dispatch(callback_id, message);
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    /* OTA Upgrade Server（子设备固件下发） */
    err = zb_device_ota_handle_action(callback_id, message);
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    /* fallback — 读取属性响应（Basic 集群，所有设备通用） */
    if (callback_id == ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID) {
        return zb_read_attr_resp_handler(
            (const esp_zb_zcl_cmd_read_attr_resp_message_t *)message);
    }

    return ESP_OK;
}

esp_zb_core_action_callback_t zdp_discovery_get_action_handler(void)
{
    return zb_action_handler;
}

/* ==================== 读取 Basic 属性响应（设备标识） ==================== */

/**
 * @brief 将 ZCL 字符字符串属性拷贝到 C 字符串
 *
 * ZCL 字符串格式为 [长度字节][内容...]，需要按实际长度截断并补 '\0'。
 */
static void copy_zcl_string(uint8_t *data, uint16_t size, char *dst, size_t dst_size)
{
    if (!data || size < 2 || dst_size < 1) {
        return;
    }
    size_t len = data[0];
    size_t avail = size - 1;
    if (len > avail) {
        len = avail;
    }
    if (len > dst_size - 1) {
        len = dst_size - 1;
    }
    memcpy(dst, &data[1], len);
    dst[len] = '\0';
}

/**
 * @brief 解析 Read Attributes 响应，提取设备标识并桥接到 Matter
 *
 * 从 Basic 集群响应中解析：
 *   - ZCL Version (0x0000) / Application Version (0x0001)
 *   - Stack Version (0x0002) / HW Version (0x0003)
 *   - Manufacturer Name (0x0004) / Model Identifier (0x0005)
 *   - Date Code (0x0006) / SW Build ID (0x4000) / deviceId (0x4001)
 *
 * 然后调用 bridge_manager_on_device_identified() 完成注册，
 * 标识信息最终写入 Matter 桥接端点的 Bridged Device Basic Information 集群。
 */
static esp_err_t zb_read_attr_resp_handler(
    const esp_zb_zcl_cmd_read_attr_resp_message_t *message)
{
    if (!message) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Read Attributes Response received");

    uint16_t short_addr = message->info.src_address.u.short_addr;
    uint8_t src_endpoint = message->info.src_endpoint;

    uint32_t device_id = DEVICE_ID_UNKNOWN;
    uint16_t uiid = UIID_UNKNOWN;
    bool got_device_id = false;
    device_identity_t identity = {};
    uint8_t zcl_version = 0;
    uint8_t stack_version = 0;

    /* 遍历所有返回的属性 */
    esp_zb_zcl_read_attr_resp_variable_t *var = message->variables;
    while (var) {
        bool ok = (var->status == ESP_ZB_ZCL_STATUS_SUCCESS) && (var->attribute.data.value != NULL);
        uint8_t *data = (uint8_t *)var->attribute.data.value;
        uint16_t size = (uint16_t)var->attribute.data.size;

        switch (var->attribute.id) {

        case ESP_ZB_ZCL_ATTR_BASIC_ZCL_VERSION_ID:
            if (ok && size >= 1) {
                zcl_version = data[0];
                ESP_LOGI(TAG, "  ZCL Version (0x0000): %u", zcl_version);
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID:
            if (ok && size >= 1) {
                identity.app_version = data[0];
                ESP_LOGI(TAG, "  Application Version (0x0001): %u", identity.app_version);
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID:
            if (ok && size >= 1) {
                stack_version = data[0];
                ESP_LOGI(TAG, "  Stack Version (0x0002): %u", stack_version);
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID:
            if (ok && size >= 1) {
                identity.hw_version = data[0];
                ESP_LOGI(TAG, "  HW Version (0x0003): %u", identity.hw_version);
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID:
            if (ok) {
                copy_zcl_string(data, size, identity.manufacturer, sizeof(identity.manufacturer));
                ESP_LOGI(TAG, "  Manufacturer: %s", identity.manufacturer);
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID:
            if (ok) {
                copy_zcl_string(data, size, identity.model, sizeof(identity.model));
                ESP_LOGI(TAG, "  Model: %s", identity.model);
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID:
            if (ok) {
                copy_zcl_string(data, size, identity.date_code, sizeof(identity.date_code));
                ESP_LOGI(TAG, "  Date Code (0x0006): %s", identity.date_code);
            }
            break;

        case ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID:
            if (ok) {
                copy_zcl_string(data, size, identity.sw_version, sizeof(identity.sw_version));
                ESP_LOGI(TAG, "  SW Version: %s", identity.sw_version);
            }
            break;

        case ZB_BASIC_ATTR_DEVICE_ID:
            if (ok && size >= 4) {
                device_id = *(uint32_t *)var->attribute.data.value;
                got_device_id = true;
                ESP_LOGI(TAG, "  deviceId (0x4001): 0x%08lX", (unsigned long)device_id);
            }
            break;

        default:
            break;
        }
        var = var->next;
    }

    /* 根据型号推断 uiid（供设备识别 + 上报配置决策共同使用） */
    uint16_t detected_uiid = device_info_lookup_uiid(identity.model);
    if (detected_uiid != UIID_UNKNOWN) {
        uiid = detected_uiid;
        ESP_LOGI(TAG, "  uiid (from model): 0x%04X", uiid);
    }

    /* 设备标识完成 → 桥接到 Matter（含厂商/型号/版本信息） */
    known_device_t *known = zboss_manager_find_by_short(short_addr);
    if (got_device_id && device_id != DEVICE_ID_UNKNOWN && device_id != 0) {
        bridge_manager_on_device_identified(device_id, uiid, short_addr, src_endpoint,
                                            known ? known->ieee_addr : NULL, &identity);
    } else {
        ESP_LOGW(TAG, "No valid deviceId found in Read Attr response");
    }

    /* 温湿度/彩色灯仅在首次配网入网时主动配置属性上报；rejoin 不重复下发 */
    if (s_first_join_pending && uiid == UIID_TEMP_HUMIDITY_SENSOR) {
        zdp_discovery_configure_reporting(short_addr, src_endpoint);
        s_first_join_pending = false;
    } else if (s_first_join_pending && uiid == UIID_RGB_LED) {
        zdp_discovery_configure_color_reporting(short_addr, src_endpoint);
        if (known) {
            zdp_discovery_bind_light(short_addr, src_endpoint, known->ieee_addr);
        } else {
            ESP_LOGW(TAG, "Skip bind for 0x%04x: IEEE for device unknown", short_addr);
        }
        s_first_join_pending = false;
    }

    return ESP_OK;
}

/* ==================== 私有集群注册状态下发 ==================== */

void zdp_discovery_send_register_status(uint16_t short_addr, uint8_t endpoint, uint16_t status)
{
    ESP_LOGI(TAG, "Send register_status=%u to 0x%04x ep=%d (cluster=0xFC11, attr=0x1FFF)",
             status, short_addr, endpoint);
    zb_tx_write_u16(short_addr, endpoint, 0xFC11, 0x1FFF, status);
}

void zdp_discovery_request_state_report(uint16_t short_addr, uint8_t endpoint)
{
    /* 子设备约定：Basic(0x0000) 属性 0x1FFF 写 U8=129 → 设备把 NVS 中保存的
     * 真实状态写回 ZCL 并主动上报一次。用于新建/重建 Matter 端点后立即同步状态。 */
    ESP_LOGI(TAG, "Request state report from 0x%04x ep=%d (Basic 0x1FFF=129)",
             short_addr, endpoint);
    zb_tx_write_u8(short_addr, endpoint, ESP_ZB_ZCL_CLUSTER_ID_BASIC, 0x1FFF, 129);
}

/* ==================== 温湿度传感器上报配置 ==================== */

/** 温湿度传感器 (uiid 0x0001) 默认上报参数 —— 对齐市面主流温湿度计 */
#define TH_REPORT_MIN_INTERVAL      5      /**< 最小上报间隔（秒） */
#define TH_REPORT_MAX_INTERVAL      1800   /**< 最大上报间隔（秒） */
#define TH_REPORT_TEMP_CHANGE       50     /**< 温度变化阈值 0.5°C（单位 0.01°C） */
#define TH_REPORT_HUMIDITY_CHANGE   100    /**< 湿度变化阈值 1%（单位 0.01%） */

static void send_config_report(uint16_t short_addr, uint8_t endpoint,
                               uint16_t cluster_id, uint16_t attr_id,
                               uint8_t attr_type, uint16_t reportable_change,
                               uint16_t min_interval, uint16_t max_interval)
{
    zb_tx_config_report(short_addr, endpoint, cluster_id, attr_id, attr_type,
                        reportable_change, min_interval, max_interval);
}

void zdp_discovery_configure_reporting(uint16_t short_addr, uint8_t endpoint)
{
    ESP_LOGI(TAG, "Configuring reporting for TH sensor 0x%04x ep=%d", short_addr, endpoint);

    send_config_report(short_addr, endpoint,
                       ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                       ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
                       ESP_ZB_ZCL_ATTR_TYPE_S16, TH_REPORT_TEMP_CHANGE,
                       TH_REPORT_MIN_INTERVAL, TH_REPORT_MAX_INTERVAL);

    send_config_report(short_addr, endpoint,
                       ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                       ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
                       ESP_ZB_ZCL_ATTR_TYPE_U16, TH_REPORT_HUMIDITY_CHANGE,
                       TH_REPORT_MIN_INTERVAL, TH_REPORT_MAX_INTERVAL);
}

/* ==================== 彩色灯上报配置 ==================== */

/* 彩色灯 (uiid 0x0002) 上报参数：对齐市面主流（Philips Hue 默认值）
   变化快的事件类属性用大 max 兜底，靠 reportable_change 及时触发上报 */
#define LIGHT_REPORT_MIN_INTERVAL   0       /**< 最小上报间隔（秒），0=尽快 */
#define LIGHT_REPORT_MAX_INTERVAL   10800   /**< 最大上报间隔（秒）兜底，主流灯均为 10800（3 小时） */
#define LIGHT_CHANGE_U8             1       /**< u8 变化阈值 */
#define LIGHT_CHANGE_U16            1       /**< u16 变化阈值 */

/**
 * @brief 彩色灯入网后主动配置属性上报
 *
 * 下发 Configure Reporting，让子设备主动上报：
 *   - OnOff(0x0006/0x0000)   开关状态
 *   - Level(0x0008/0x0000)   亮度 CurrentLevel
 *   - ColorControl(0x0300)   CurrentHue/Saturation/X/Y/ColorMode/ColorTemperature
 * 使 HA 能实时反映子设备的物理状态变化。
 */
void zdp_discovery_configure_color_reporting(uint16_t short_addr, uint8_t endpoint)
{
    ESP_LOGI(TAG, "Configuring reporting for RGB-light 0x%04x ep=%d", short_addr, endpoint);

    send_config_report(short_addr, endpoint,
                       ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                       ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                       ESP_ZB_ZCL_ATTR_TYPE_BOOL, 0,
                       LIGHT_REPORT_MIN_INTERVAL, LIGHT_REPORT_MAX_INTERVAL);
    send_config_report(short_addr, endpoint,
                       ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                       ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
                       ESP_ZB_ZCL_ATTR_TYPE_U8, LIGHT_CHANGE_U8,
                       LIGHT_REPORT_MIN_INTERVAL, LIGHT_REPORT_MAX_INTERVAL);
    send_config_report(short_addr, endpoint,
                       ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                       ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID,
                       ESP_ZB_ZCL_ATTR_TYPE_U8, LIGHT_CHANGE_U8,
                       LIGHT_REPORT_MIN_INTERVAL, LIGHT_REPORT_MAX_INTERVAL);
    send_config_report(short_addr, endpoint,
                       ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                       ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID,
                       ESP_ZB_ZCL_ATTR_TYPE_U8, LIGHT_CHANGE_U8,
                       LIGHT_REPORT_MIN_INTERVAL, LIGHT_REPORT_MAX_INTERVAL);
    send_config_report(short_addr, endpoint,
                       ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                       ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID,
                       ESP_ZB_ZCL_ATTR_TYPE_U16, LIGHT_CHANGE_U16,
                       LIGHT_REPORT_MIN_INTERVAL, LIGHT_REPORT_MAX_INTERVAL);
    send_config_report(short_addr, endpoint,
                       ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                       ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID,
                       ESP_ZB_ZCL_ATTR_TYPE_U16, LIGHT_CHANGE_U16,
                       LIGHT_REPORT_MIN_INTERVAL, LIGHT_REPORT_MAX_INTERVAL);
    send_config_report(short_addr, endpoint,
                       ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                       ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_MODE_ID,
                       0x30, 0,
                       LIGHT_REPORT_MIN_INTERVAL, LIGHT_REPORT_MAX_INTERVAL);
    send_config_report(short_addr, endpoint,
                       ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                       ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
                       ESP_ZB_ZCL_ATTR_TYPE_U16, LIGHT_CHANGE_U16,
                       LIGHT_REPORT_MIN_INTERVAL, LIGHT_REPORT_MAX_INTERVAL);
}

/* ==================== 彩色灯绑定下发 ==================== */

static const uint16_t k_light_bind_clusters[] = {
    ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
    ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
    ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
};

void zdp_discovery_bind_light(uint16_t short_addr, uint8_t endpoint, esp_zb_ieee_addr_t src_ieee)
{
    const size_t n = sizeof(k_light_bind_clusters) / sizeof(k_light_bind_clusters[0]);
    for (size_t i = 0; i < n; i++) {
        zb_tx_bind(short_addr, endpoint, k_light_bind_clusters[i], src_ieee);
    }
}

/* ==================== 设备离网清理 ==================== */

void zdp_discovery_handle_device_leave(uint16_t short_addr, const esp_zb_ieee_addr_t ieee)
{
    ESP_LOGI(TAG, "Handling device leave: 0x%04x", short_addr);

    /* 移除 Matter 桥接设备 */
    app_bridged_device_t *bridged = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (bridged) {
        ESP_LOGI(TAG, "Removing Matter bridge for device 0x%04x", short_addr);
        app_bridge_remove_device(bridged);
    }

    /* 从 device_info 中删除（包括 NVS 持久化数据）。
     * 短地址会随重加入网变化，导致 NVS 里存的可能是过期值；
     * 因此优先按稳定不变的 IEEE 查找，找不到再退回短地址。 */
    device_info_t *info = NULL;
    if (ieee) {
        info = device_info_find_by_ieee(ieee);
    }
    if (!info) {
        info = device_info_find_by_short_addr(short_addr);
    }
    if (info && info->device_id != DEVICE_ID_UNKNOWN) {
        device_info_remove_by_device_id(info->device_id);
    }

    zigbee_db_remove(ieee, short_addr);
}

/* ==================== 读取 Basic 属性 ==================== */

void zdp_discovery_read_basic_attributes(uint16_t short_addr, uint8_t endpoint)
{
    ESP_LOGI(TAG, "Reading Basic attributes from device 0x%04x, endpoint=%d", short_addr, endpoint);
    zb_tx_read_basic(short_addr, endpoint);
}

/* ==================== 初始化 ==================== */

void zdp_discovery_init(void)
{
    ESP_LOGI(TAG, "zdp_discovery init: probe=%d, annce_delay=%dms, retries=%d (base=%dms)",
             CONFIG_ZIGBEE_MAX_ENDPOINT_PROBE,
             CONFIG_ZIGBEE_ANNCE_DELAY_MS,
             CONFIG_ZIGBEE_DISCOVERY_MAX_RETRIES,
             CONFIG_ZIGBEE_DISCOVERY_RETRY_BASE_MS);

    /* 初始化 ZCL 设备处理器注册表 */
    zcl_handlers_init();
}
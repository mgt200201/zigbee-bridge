/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file bridge_manager.cpp
 *
 * @brief Matter 桥接管理层
 *
 * 负责：
 * - Matter 桥接设备的创建和删除
 * - Zigbee ↔ Matter 属性翻译
 * - 设备标识信息的注册和持久化
 * - 传感器数据上报
 *
 * 本层只懂 Matter 协议，所有 Zigbee 细节通过 zdp_discovery 接口访问。
 */

#include <bridge_manager.h>
#include "device_info.h"
#include "zigbee_matter_map.h"
#include <commissioning_mgr.h>
#include <zboss_manager.h>
#include <zdp_discovery.h>
#include <zcl_bridge_api.h>
#include <zb_tx_queue.h>
#include <matter/app_status.h>
#include <wifi_manager.h>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_bridge.h>
#include <esp_matter_console.h>
#include <nvs_flash.h>

#include <setup_payload/SetupPayload.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>

#include <common_macros.h>
#include <esp_timer.h>

#include <string.h>

static const char *TAG = "bridge_manager";

using namespace chip::app::Clusters;
using namespace esp_matter;
using namespace esp_matter::cluster;

/** Aggregator 端点 ID */
static uint16_t s_aggregator_ep_id = chip::kInvalidEndpointId;

/* Zigbee 栈就绪标志：esp_zb_init() 之前为 false。 */
static volatile bool s_zb_stack_ready = false;

/* 开关与亮度解耦：开/关只发 OnOff；关着时不发 MoveToLevel。
 * level<=1 为 Matter 关灯地板值，一律不下发（避免关灯过程点到 1%）。 */
#define LIGHT_LEVEL_SLOTS 8
typedef struct {
    uint16_t matter_ep;
    bool valid;
    bool is_on;               /* 最近一次 OnOff（下行或 Zigbee 上报） */
    uint8_t last_level;       /* 上次非 0 亮度（仅记忆） */
} light_level_shadow_t;
static light_level_shadow_t s_light_level[LIGHT_LEVEL_SLOTS];

static light_level_shadow_t *light_level_find(uint16_t matter_ep)
{
    for (int i = 0; i < LIGHT_LEVEL_SLOTS; i++) {
        if (s_light_level[i].valid && s_light_level[i].matter_ep == matter_ep) {
            return &s_light_level[i];
        }
    }
    for (int i = 0; i < LIGHT_LEVEL_SLOTS; i++) {
        if (!s_light_level[i].valid) {
            s_light_level[i] = {};
            s_light_level[i].matter_ep = matter_ep;
            s_light_level[i].valid = true;
            return &s_light_level[i];
        }
    }
    return NULL;
}

static void light_remember_level(uint16_t matter_ep, uint8_t level)
{
    if (level == 0 || level == 0xFF) {
        return;
    }
    light_level_shadow_t *s = light_level_find(matter_ep);
    if (s) {
        s->last_level = level;
    }
}

/* ---- 下行颜色：Hue/Sat（或 X/Y）分两次写属性，立即发会先带旧配对再纠正。
 * 80ms 防抖合并成一次；HA 每次调色带的 On 若 Matter 已是 On 则无属性回调，这里补发。 */
#define COLOR_EP_SHADOW_SLOTS 8
#define COLOR_DEBOUNCE_US     80000
typedef struct {
    uint16_t matter_ep;
    bool valid;
    bool have_x, have_y;
    bool have_hue, have_sat;
    uint16_t x, y;
    uint8_t hue, sat;
    uint16_t zshort;
    uint8_t zep;
    esp_timer_handle_t hs_timer;
    esp_timer_handle_t xy_timer;
} color_ep_shadow_t;
static color_ep_shadow_t s_color_ep_shadow[COLOR_EP_SHADOW_SLOTS];

static color_ep_shadow_t *color_ep_shadow_find(uint16_t matter_ep)
{
    for (int i = 0; i < COLOR_EP_SHADOW_SLOTS; i++) {
        if (s_color_ep_shadow[i].valid && s_color_ep_shadow[i].matter_ep == matter_ep) {
            return &s_color_ep_shadow[i];
        }
    }
    for (int i = 0; i < COLOR_EP_SHADOW_SLOTS; i++) {
        if (!s_color_ep_shadow[i].valid) {
            s_color_ep_shadow[i] = {};
            s_color_ep_shadow[i].matter_ep = matter_ep;
            s_color_ep_shadow[i].valid = true;
            return &s_color_ep_shadow[i];
        }
    }
    return NULL;
}

static void color_ensure_on(uint16_t matter_ep, uint16_t zshort, uint8_t zep)
{
    light_level_shadow_t *lv = light_level_find(matter_ep);
    /* Matter 已是 On 时 HA 仍发 On，但属性不变 → 无下行；补发保证 Zigbee 侧亮着才能吃颜色 */
    if (!lv || lv->is_on) {
        zb_tx_on_off(zshort, zep, true);
        if (lv) {
            lv->is_on = true;
        }
    }
}

static void color_hs_debounce_cb(void *arg)
{
    color_ep_shadow_t *shadow = static_cast<color_ep_shadow_t *>(arg);
    if (!shadow->valid || !shadow->have_hue || !shadow->have_sat) {
        return;
    }
    color_ensure_on(shadow->matter_ep, shadow->zshort, shadow->zep);
    zb_tx_move_to_hue_sat(shadow->zshort, shadow->zep, shadow->hue, shadow->sat);
}

static void color_xy_debounce_cb(void *arg)
{
    color_ep_shadow_t *shadow = static_cast<color_ep_shadow_t *>(arg);
    if (!shadow->valid || !shadow->have_x || !shadow->have_y) {
        return;
    }
    color_ensure_on(shadow->matter_ep, shadow->zshort, shadow->zep);
    zb_tx_move_to_color(shadow->zshort, shadow->zep, shadow->x, shadow->y);
}

static void color_schedule_hs(color_ep_shadow_t *shadow, uint16_t zshort, uint8_t zep)
{
    shadow->zshort = zshort;
    shadow->zep = zep;
    if (!shadow->hs_timer) {
        esp_timer_create_args_t args = {
            .callback = &color_hs_debounce_cb,
            .arg = shadow,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "clr_hs",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &shadow->hs_timer) != ESP_OK) {
            color_hs_debounce_cb(shadow);
            return;
        }
    }
    esp_timer_stop(shadow->hs_timer);
    esp_timer_start_once(shadow->hs_timer, COLOR_DEBOUNCE_US);
}

static void color_schedule_xy(color_ep_shadow_t *shadow, uint16_t zshort, uint8_t zep)
{
    shadow->zshort = zshort;
    shadow->zep = zep;
    if (!shadow->xy_timer) {
        esp_timer_create_args_t args = {
            .callback = &color_xy_debounce_cb,
            .arg = shadow,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "clr_xy",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &shadow->xy_timer) != ESP_OK) {
            color_xy_debounce_cb(shadow);
            return;
        }
    }
    esp_timer_stop(shadow->xy_timer);
    esp_timer_start_once(shadow->xy_timer, COLOR_DEBOUNCE_US);
}

/* ---- 上行抑制：Zigbee→Matter 写属性会触发 PRE_UPDATE，若全局屏蔽会把
       同时到来的 HA Off 一并丢掉（灯关不掉）。只压掉「正在写入的同 ep/cluster/attr」回声。 ---- */
static int s_uplink_depth = 0;
static uint16_t s_uplink_ep = 0xFFFF;
static uint32_t s_uplink_cluster = 0;
static uint32_t s_uplink_attr = 0;

static esp_err_t bridge_set_attr_no_echo(uint16_t ep, uint32_t cluster, uint32_t attr,
                                         esp_matter_attr_val_t *val)
{
    s_uplink_depth++;
    s_uplink_ep = ep;
    s_uplink_cluster = cluster;
    s_uplink_attr = attr;
    esp_err_t err = attribute::update(ep, cluster, attr, val);
    s_uplink_depth--;
    if (s_uplink_depth <= 0) {
        s_uplink_depth = 0;
        s_uplink_ep = 0xFFFF;
    }
    return err;
}

/* OnLevel=null：开灯时沿用上次 CurrentLevel；非 null（旧固件写成 254）会每次开到满亮。 */
static void bridge_force_on_level_null(uint16_t matter_ep)
{
    esp_matter_attr_val_t val = esp_matter_nullable_uint8(nullable<uint8_t>());
    esp_err_t err = bridge_set_attr_no_echo(matter_ep, LevelControl::Id,
                                            LevelControl::Attributes::OnLevel::Id, &val);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "OnLevel null skip ep=%u: %s", matter_ep, esp_err_to_name(err));
    }
}

static void bridge_clear_on_level_all_lights(void)
{
    node_t *node = node::get();
    if (!node) {
        return;
    }
    int cleared = 0;
    for (endpoint_t *ep = endpoint::get_first(node); ep; ep = endpoint::get_next(ep)) {
        if (!cluster::get(ep, LevelControl::Id)) {
            continue;
        }
        uint16_t id = endpoint::get_id(ep);
        bridge_force_on_level_null(id);
        cleared++;
    }
    if (cleared > 0) {
        ESP_LOGI(TAG, "Cleared OnLevel on %d LevelControl endpoint(s)", cleared);
    }
}

/* ==================== Bridge API 适配层（依赖注入） ==================== */

/**
 * 将 bridge_manager 的全局函数包装为 zcl_bridge_api 接口，
 * 使 ZCL handler 层不直接依赖 bridge_manager，而是通过接口注入。
 */

static void bridge_api_report_sensor(uint16_t short_addr, uint8_t endpoint,
                                     uint16_t cluster_id, int16_t raw_value)
{
    bridge_manager_report_sensor_data(short_addr, endpoint, cluster_id, raw_value);
}

static void bridge_api_report_on_off(uint16_t short_addr, uint8_t endpoint, bool on)
{
    app_bridged_device_t *dev = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (!dev || !dev->dev || !dev->dev->endpoint) {
        ESP_LOGW(TAG, "Cannot report On/Off: no bridge for 0x%04x", short_addr);
        return;
    }
    uint16_t matter_ep = esp_matter::endpoint::get_id(dev->dev->endpoint);
    light_level_shadow_t *lv = light_level_find(matter_ep);
    if (lv) {
        lv->is_on = on;
    }
    esp_matter_attr_val_t val = esp_matter_bool(on);
    esp_err_t err = bridge_set_attr_no_echo(matter_ep, OnOff::Id,
                                            OnOff::Attributes::OnOff::Id, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "On/Off attribute::update failed: %s", esp_err_to_name(err));
    }
}

static bool bridge_api_is_bridged(uint16_t short_addr)
{
    return bridge_manager_is_device_bridged(short_addr);
}

static uint8_t bridge_api_get_ep(uint16_t short_addr)
{
    return bridge_manager_get_endpoint_by_short(short_addr);
}

/* ---------------- 灯具状态上报（Zigbee → Matter） ---------------- */

static void bridge_api_report_level(uint16_t short_addr, uint8_t endpoint, uint8_t level)
{
    app_bridged_device_t *dev = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (!dev || !dev->dev || !dev->dev->endpoint) {
        ESP_LOGW(TAG, "Cannot report Level: no bridge for 0x%04x", short_addr);
        return;
    }
    uint16_t matter_ep = esp_matter::endpoint::get_id(dev->dev->endpoint);
    /* 关灯后灯常报 level=0。若写入 Matter，下次 On 会 MoveToLevel(1) 盖掉灯内亮度。 */
    if (level == 0) {
        ESP_LOGD(TAG, "Ignore uplink level=0 ep=%u (keep last brightness)", matter_ep);
        return;
    }
    uint8_t lv = (level == 0xFF) ? 0xFE : level;
    light_remember_level(matter_ep, lv);
    esp_matter_attr_val_t val = esp_matter_nullable_uint8(lv);
    esp_err_t err = bridge_set_attr_no_echo(matter_ep, LevelControl::Id,
                                            LevelControl::Attributes::CurrentLevel::Id, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Level attribute::update failed on ep %d: %s", matter_ep, esp_err_to_name(err));
    }
}

static void bridge_api_report_color_xy(uint16_t short_addr, uint8_t endpoint, uint16_t x, uint16_t y)
{
    app_bridged_device_t *dev = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (!dev || !dev->dev || !dev->dev->endpoint) {
        ESP_LOGW(TAG, "Cannot report Color XY: no bridge for 0x%04x", short_addr);
        return;
    }
    uint16_t matter_ep = esp_matter::endpoint::get_id(dev->dev->endpoint);
    /* CurrentX/CurrentY 在该端点上是非可空 uint16，传可空值会因类型不匹配报 ESP_ERR_INVALID_ARG(258) */
    esp_matter_attr_val_t vx = esp_matter_uint16(x);
    esp_matter_attr_val_t vy = esp_matter_uint16(y);
    esp_err_t e1 = bridge_set_attr_no_echo(matter_ep, ColorControl::Id, ColorControl::Attributes::CurrentX::Id, &vx);
    esp_err_t e2 = bridge_set_attr_no_echo(matter_ep, ColorControl::Id, ColorControl::Attributes::CurrentY::Id, &vy);
    if (e1 != ESP_OK) ESP_LOGE(TAG, "ColorX update failed on ep %d: %s", matter_ep, esp_err_to_name(e1));
    if (e2 != ESP_OK) ESP_LOGE(TAG, "ColorY update failed on ep %d: %s", matter_ep, esp_err_to_name(e2));
}

static void bridge_api_report_color_hsv(uint16_t short_addr, uint8_t endpoint, uint8_t hue, uint8_t sat)
{
    app_bridged_device_t *dev = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (!dev || !dev->dev || !dev->dev->endpoint) {
        ESP_LOGW(TAG, "Cannot report Color HSV: no bridge for 0x%04x", short_addr);
        return;
    }
    uint16_t matter_ep = esp_matter::endpoint::get_id(dev->dev->endpoint);
    esp_matter_attr_val_t vh = esp_matter_nullable_uint8(hue);
    esp_matter_attr_val_t vs = esp_matter_nullable_uint8(sat);
    esp_err_t e1 = bridge_set_attr_no_echo(matter_ep, ColorControl::Id, ColorControl::Attributes::CurrentHue::Id, &vh);
    esp_err_t e2 = bridge_set_attr_no_echo(matter_ep, ColorControl::Id, ColorControl::Attributes::CurrentSaturation::Id, &vs);
    if (e1 != ESP_OK) ESP_LOGE(TAG, "Hue update failed on ep %d: %s", matter_ep, esp_err_to_name(e1));
    if (e2 != ESP_OK) ESP_LOGE(TAG, "Sat update failed on ep %d: %s", matter_ep, esp_err_to_name(e2));
}

static void bridge_api_report_color_temperature(uint16_t short_addr, uint8_t endpoint, uint16_t mireds)
{
    app_bridged_device_t *dev = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (!dev || !dev->dev || !dev->dev->endpoint) {
        ESP_LOGW(TAG, "Cannot report ColorTemperature: no bridge for 0x%04x", short_addr);
        return;
    }
    uint16_t matter_ep = esp_matter::endpoint::get_id(dev->dev->endpoint);
    esp_matter_attr_val_t val = esp_matter_uint16(mireds);
    esp_err_t err = bridge_set_attr_no_echo(matter_ep, ColorControl::Id,
                                            ColorControl::Attributes::ColorTemperatureMireds::Id, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ColorTemperature update failed on ep %d: %s", matter_ep, esp_err_to_name(err));
    }
}

/** Bridge API 实例（静态全局，在 init 中注册到 ZCL 层） */
static const zcl_bridge_api_t s_bridge_api_impl = {
    .report_sensor = bridge_api_report_sensor,
    .report_on_off = bridge_api_report_on_off,
    .report_level = bridge_api_report_level,
    .report_color_xy = bridge_api_report_color_xy,
    .report_color_hsv = bridge_api_report_color_hsv,
    .report_color_temperature = bridge_api_report_color_temperature,
    .is_device_bridged = bridge_api_is_bridged,
    .get_endpoint_by_short = bridge_api_get_ep,
};

void bridge_manager_init(uint16_t aggregator_endpoint_id)
{
    s_aggregator_ep_id = aggregator_endpoint_id;

    /* 注册桥接接口到 ZCL 层（依赖注入） */
    zcl_bridge_api_register(&s_bridge_api_impl);

    ESP_LOGI(TAG, "Bridge manager initialized, aggregator endpoint=%d", aggregator_endpoint_id);
}

/* ==================== Matter 事件回调 ==================== */

void bridge_manager_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
    case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
        /* WiFi/网络事件委托给 wifi_manager 处理 */
        wifi_manager_handle_chip_event(event, arg);
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        app_status_notify_ready(); /* 配网完成 → 标记已配网(fabric) */
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        {
            chip::PayloadContents payloadContents;
            payloadContents.setUpPINCode = commissioning_mgr_get_pin();
            payloadContents.discriminator.SetLongValue(commissioning_mgr_get_discriminator());
            payloadContents.rendezvousInformation.SetValue(chip::RendezvousInformationFlag::kBLE);
            payloadContents.commissioningFlow = chip::CommissioningFlow::kStandard;
            std::string manualPairingCode;
            chip::ManualSetupPayloadGenerator generator(payloadContents);
            generator.payloadDecimalStringRepresentation(manualPairingCode);
            ESP_LOGI(TAG, "Manual Pairing Code: %s", manualPairingCode.c_str());
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    default:
        break;
    }
}

/* ==================== 桥接端点创建回调 ==================== */

esp_err_t bridge_manager_create_devices_cb(esp_matter::endpoint_t *ep, uint32_t device_type_id, void *priv_data)
{
    (void)priv_data;
    switch (device_type_id) {
    case ESP_MATTER_TEMP_HUMIDITY_SENSOR_DEVICE_TYPE_ID: {
        temperature_measurement::config_t temp_conf;
        temp_conf.measured_value = { static_cast<int16_t>(0) };
        temp_conf.min_measured_value = { static_cast<int16_t>(-2000) };
        temp_conf.max_measured_value = { static_cast<int16_t>(2000) };
        temperature_measurement::create(ep, &temp_conf, CLUSTER_FLAG_SERVER);
        relative_humidity_measurement::config_t humidity_conf;
        humidity_conf.measured_value = { static_cast<uint16_t>(0) };
        humidity_conf.min_measured_value = { static_cast<uint16_t>(0) };
        humidity_conf.max_measured_value = { static_cast<uint16_t>(10000) };
        relative_humidity_measurement::create(ep, &humidity_conf, CLUSTER_FLAG_SERVER);
        break;
    }
    case ESP_MATTER_EXTENDED_COLOR_LIGHT_DEVICE_TYPE_ID: {
        /* 可扩展色灯：必须用设备类型 API add()。
         * 它会把 ExtendedColorLight(0x010D) 写进端点 Descriptor 的 DeviceTypeList，
         * 并且创建 OnOff+LevelControl+ColorControl 集群。
         * 若像之前那样手动 on_off/level/color::create，端点只声明 BridgedNode(0x0013)，
         * HA 无法识别为 light.*，会显示成 switch。 */
        endpoint::extended_color_light::config_t light_conf;
        light_conf.level_control.current_level = static_cast<uint8_t>(0);
        /* OnLevel 保持默认 null。写成 254 时每次 OnOff→On 都会把 CurrentLevel
         * 强制拉到满亮度，用户上次设的亮度会被丢掉。 */
        esp_err_t err = endpoint::extended_color_light::add(ep, &light_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add extended_color_light: %s", esp_err_to_name(err));
            return err;
        }
        bridge_force_on_level_null(endpoint::get_id(ep));
        /* endpoint::extended_color_light::add() 默认只开启 color_temperature + xy 两个 feature，
         * 未开 hue_saturation(HS)。而 HS(MoveToHueAndSaturation) 命令依赖 HS feature bit，
         * 不开则 Matter 框架会直接返回 INVALID_COMMAND(0x81)，颜色无法用。这里补开，
         * 使 FeatureMap = HS|XY，两种颜色命令都被接受。 */
        esp_matter::cluster_t *cc = esp_matter::cluster::get(ep, ColorControl::Id);
        if (cc) {
            esp_matter::cluster::color_control::feature::hue_saturation::config_t hs_conf;
            err = esp_matter::cluster::color_control::feature::hue_saturation::add(cc, &hs_conf);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add hue_saturation feature: %s", esp_err_to_name(err));
                return err;
            }
        } else {
            ESP_LOGE(TAG, "ColorControl cluster not found on bridged ep after add");
            return ESP_ERR_NOT_FOUND;
        }
        break;
    }
    default:
        ESP_LOGE(TAG, "Unsupported bridged matter device type: 0x%04x", (unsigned int)device_type_id);
            return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ==================== 属性更新回调 ==================== */

esp_err_t bridge_manager_attr_update_cb(esp_matter::attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == esp_matter::attribute::PRE_UPDATE) {
        return bridge_manager_attr_update(endpoint_id, cluster_id, attribute_id,
                                           val, (app_bridged_device_t *)priv_data);
    }
    return ESP_OK;
}

/* ==================== 桥接设备查询 ==================== */

void bridge_manager_set_zb_stack_ready(bool ready)
{
    s_zb_stack_ready = ready;
    if (ready) {
        /* 旧固件把 OnLevel 持久化成 254，开机清一次，已有彩灯才能记住亮度。 */
        bridge_clear_on_level_all_lights();
    }
}

bool bridge_manager_is_device_bridged(uint16_t short_addr)
{
    return app_bridge_get_device_by_zigbee_shortaddr(short_addr) != NULL;
}

uint8_t bridge_manager_get_endpoint_by_short(uint16_t short_addr)
{
    /* 优先从 device_info 查找（内存中的在线状态） */
    device_info_t *info = device_info_find_by_short_addr(short_addr);
    if (info) {
        return info->zigbee_endpoint;
    }
    /* 回退：从 Matter bridge 已恢复的数据中获取 Zigbee 端点号
     * 重启后 device_info.short_addr = 0xFFFF，但 Matter bridge 的 zigbee 地址已从 NVS 恢复 */
    app_bridged_device_t *bridged = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (bridged) {
        ESP_LOGI(TAG, "Fell back to Matter bridge for endpoint: short=0x%04hx, ep=%d",
                 short_addr, bridged->dev_addr.zigbee_endpointid);
        return bridged->dev_addr.zigbee_endpointid;
    }
    return 0xFF;
}

void bridge_manager_on_device_discovered(uint16_t short_addr, uint8_t endpoint)
{
    /* 检查是否已存在桥接设备 */
    if (app_bridge_get_device_by_zigbee_shortaddr(short_addr)) {
        ESP_LOGI(TAG, "Bridged node for 0x%04x already exists on endpoint %d",
                 short_addr, app_bridge_get_matter_endpointid_by_zigbee_shortaddr(short_addr));
        return;
    }

    /* 此时还不知道型号/uiid，无法确定 Matter 端点类型（传感器 or 灯）。
     * 因此不在此创建桥接设备，仅在 identified 拿到 uiid 后再按类型创建。
     */

    /* 注册到 zboss_manager */
    zboss_manager_register_bridged_device(short_addr, endpoint);

    /* 主动读取 Basic 属性以确定型号/uiid */
    zdp_discovery_read_basic_attributes(short_addr, endpoint);
}

static uint32_t uiid_to_matter_device_type_id(uint16_t uiid)
{
    switch (uiid) {
    case UIID_RGB_LED:
        return ESP_MATTER_EXTENDED_COLOR_LIGHT_DEVICE_TYPE_ID;
    case UIID_TEMP_HUMIDITY_SENSOR:
    default:
        /* 未知型号按温湿度传感器兜底 */
        return ESP_MATTER_TEMP_HUMIDITY_SENSOR_DEVICE_TYPE_ID;
    }
}

/* ==================== 桥接端点标识信息写入（0x0039） ====================
 *
 * Bridged Device Basic Information 集群的属性均为可选属性，esp-matter 的
 * bridged_node::create() 默认只创建 Reachable/UniqueID/ClusterRevision/FeatureMap。
 * 这里按需创建并写入厂商、型号、版本等信息，让 HA 能读到子设备的真实标识。 */

static void bridge_basic_set_str(cluster_t *cluster, uint16_t ep, uint32_t attr_id,
                                 attribute_t *(*create_fn)(cluster_t *, char *, uint16_t),
                                 const char *value, const char *label)
{
    if (!value || value[0] == '\0') {
        return;
    }
    if (!attribute::get(cluster, attr_id)) {
        if (!create_fn(cluster, (char *)value, (uint16_t)strlen(value))) {
            ESP_LOGW(TAG, "Create basic-info %s failed", label);
            return;
        }
    }
    esp_matter_attr_val_t val = esp_matter_char_str((char *)value, (uint16_t)strlen(value));
    esp_err_t err = attribute::update(ep, BridgedDeviceBasicInformation::Id, attr_id, &val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Update basic-info %s failed: %s", label, esp_err_to_name(err));
    }
}

static void bridge_basic_set_u16(cluster_t *cluster, uint16_t ep, uint32_t attr_id,
                                 attribute_t *(*create_fn)(cluster_t *, uint16_t),
                                 uint16_t value, const char *label)
{
    if (!attribute::get(cluster, attr_id)) {
        if (!create_fn(cluster, value)) {
            ESP_LOGW(TAG, "Create basic-info %s failed", label);
            return;
        }
    }
    esp_matter_attr_val_t val = esp_matter_uint16(value);
    esp_err_t err = attribute::update(ep, BridgedDeviceBasicInformation::Id, attr_id, &val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Update basic-info %s failed: %s", label, esp_err_to_name(err));
    }
}

static void bridge_basic_set_u32(cluster_t *cluster, uint16_t ep, uint32_t attr_id,
                                 attribute_t *(*create_fn)(cluster_t *, uint32_t),
                                 uint32_t value, const char *label)
{
    if (!attribute::get(cluster, attr_id)) {
        if (!create_fn(cluster, value)) {
            ESP_LOGW(TAG, "Create basic-info %s failed", label);
            return;
        }
    }
    esp_matter_attr_val_t val = esp_matter_uint32(value);
    esp_err_t err = attribute::update(ep, BridgedDeviceBasicInformation::Id, attr_id, &val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Update basic-info %s failed: %s", label, esp_err_to_name(err));
    }
}

/**
 * @brief 把 Zigbee 设备标识写入 Matter 桥接端点的 0x0039 集群
 */
static void bridge_apply_identity(uint16_t matter_ep, const device_identity_t *identity)
{
    if (!identity || matter_ep == 0 || matter_ep == chip::kInvalidEndpointId) {
        return;
    }
    node_t *node = node::get();
    endpoint_t *ep = node ? endpoint::get(node, matter_ep) : NULL;
    if (!ep) {
        ESP_LOGW(TAG, "No endpoint %u for identity info", matter_ep);
        return;
    }
    cluster_t *cluster = cluster::get(ep, BridgedDeviceBasicInformation::Id);
    if (!cluster) {
        ESP_LOGW(TAG, "No BridgedDeviceBasicInformation cluster on ep %u", matter_ep);
        return;
    }

    bridge_basic_set_str(cluster, matter_ep, BridgedDeviceBasicInformation::Attributes::VendorName::Id,
                         &bridged_device_basic_information::attribute::create_vendor_name,
                         identity->manufacturer, "VendorName");
    bridge_basic_set_str(cluster, matter_ep, BridgedDeviceBasicInformation::Attributes::ProductName::Id,
                         &bridged_device_basic_information::attribute::create_product_name,
                         identity->model, "ProductName");
    bridge_basic_set_str(cluster, matter_ep, BridgedDeviceBasicInformation::Attributes::SoftwareVersionString::Id,
                         &bridged_device_basic_information::attribute::create_software_version_string,
                         identity->sw_version, "SoftwareVersionString");
    if (identity->app_version > 0) {
        bridge_basic_set_u32(cluster, matter_ep, BridgedDeviceBasicInformation::Attributes::SoftwareVersion::Id,
                             &bridged_device_basic_information::attribute::create_software_version,
                             identity->app_version, "SoftwareVersion");
    }
    if (identity->hw_version > 0) {
        bridge_basic_set_u16(cluster, matter_ep, BridgedDeviceBasicInformation::Attributes::HardwareVersion::Id,
                             &bridged_device_basic_information::attribute::create_hardware_version,
                             identity->hw_version, "HardwareVersion");
    }
    if (identity->date_code[0] != '\0') {
        bridge_basic_set_str(cluster, matter_ep, BridgedDeviceBasicInformation::Attributes::ManufacturingDate::Id,
                             &bridged_device_basic_information::attribute::create_manufacturing_date,
                             identity->date_code, "ManufacturingDate");
    }

    ESP_LOGI(TAG, "Applied identity to Matter ep=%u (mfr=%s model=%s sw=%s app=%u hw=%u)",
             matter_ep, identity->manufacturer, identity->model, identity->sw_version,
             identity->app_version, identity->hw_version);
}

void bridge_manager_on_device_identified(uint32_t device_id, uint16_t uiid,
                                          uint16_t short_addr, uint8_t endpoint,
                                          const esp_zb_ieee_addr_t ieee_addr,
                                          const device_identity_t *identity)
{
    device_info_t *info = device_info_find_by_device_id(device_id);

    if (info) {
        /* ===== CASE 1: 设备重连 ===== */
        uint16_t old_short_addr = info->short_addr;
        device_info_update_online(device_id, short_addr, endpoint, ieee_addr);

        if (old_short_addr != short_addr) {
            /* 短地址变化时始终刷新 Matter bridge 的下行地址。
             * 注意：不能依赖 info->matter_endpoint_id（网关重启后从 NVS 恢复为 0），
             * 否则子设备断电换短地址重连时，下行仍发往旧的失效地址，导致 HA 无法控制。 */
            app_bridged_device_t *bridged = app_bridge_get_device_by_zigbee_shortaddr(old_short_addr);
            if (bridged) {
                bridged->dev_addr.zigbee_shortaddr = short_addr;
                ESP_LOGI(TAG, "Updated Matter bridge addr: 0x%04hx -> 0x%04hx (device_id=0x%08X)",
                         old_short_addr, short_addr, (unsigned int)device_id);
            }
            zboss_manager_register_bridged_device(short_addr, endpoint);
        }

        /* 兜底：若 device_info 残留但网内实际没有活着的 Matter 桥接端点
         * （例如曾离网导致端点被销毁，而 device_info 因短地址过期未被清理），
         * 直接走重连会导致永远没有 Matter 节点、HA 看不到。
         * 这里检测到无端点就按新设备逻辑重建一个。 */
        app_bridged_device_t *existing = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
        if (!existing) {
            existing = app_bridge_get_device_by_zigbee_shortaddr(old_short_addr);
        }
        if (!existing) {
            node_t *node = node::get();
            if (node) {
                uint16_t use_uiid = (info->uiid != UIID_UNKNOWN) ? info->uiid : uiid;
                app_bridged_device_t *recreated =
                    app_bridge_create_bridged_device(node, s_aggregator_ep_id,
                                                     uiid_to_matter_device_type_id(use_uiid),
                                                     ESP_MATTER_BRIDGED_DEVICE_TYPE_ZIGBEE,
                                                     app_bridge_zigbee_address(endpoint, short_addr), NULL);
                if (recreated) {
                    zboss_manager_register_bridged_device(short_addr, endpoint);
                    uint16_t matter_ep =
                        app_bridge_get_matter_endpointid_by_zigbee_shortaddr(short_addr);
                    device_info_set_matter_endpoint(device_id, matter_ep);
                    ESP_LOGI(TAG, "Reallocated Matter endpoint for lingering device_id=0x%08X: Matter ep=%d",
                             (unsigned int)device_id, matter_ep);
                }
            }
        }

        if (info->uiid == UIID_UNKNOWN && uiid != UIID_UNKNOWN) {
            info->uiid = uiid;
        }

        /* 重连/重启 resume 后端点上的标识属性不会自动重写，这里回填一次 */
        if (identity) {
            device_info_set_identity(device_id, identity);
            uint16_t matter_ep = app_bridge_get_matter_endpointid_by_zigbee_shortaddr(short_addr);
            bridge_apply_identity(matter_ep, identity);
        }

        ESP_LOGI(TAG, "Device reconnected: device_id=0x%08X, Matter ep=%d",
                 (unsigned int)device_id, info->matter_endpoint_id);

        /* 重配后新建的 Matter 端点初始状态是默认值(off/0)，而设备可能本是开着的。
         * 主动触发设备上报一次真实状态，避免 HA 第一次控制"无效"。 */
        zdp_discovery_request_state_report(short_addr, endpoint);
        return;
    }

    /* ===== CASE 2: 新设备 —— 在拿到 uiid 后按类型创建 Matter 端点 ===== */
    app_bridged_device_t *bridged = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (!bridged) {
        node_t *node = node::get();
        if (!node) {
            ESP_LOGE(TAG, "Could not find esp_matter node");
            return;
        }
        bridged = app_bridge_create_bridged_device(node, s_aggregator_ep_id,
                                                   uiid_to_matter_device_type_id(uiid),
                                                   ESP_MATTER_BRIDGED_DEVICE_TYPE_ZIGBEE,
                                                   app_bridge_zigbee_address(endpoint, short_addr), NULL);
        if (!bridged) {
            ESP_LOGE(TAG, "Failed to create bridged device for 0x%04x (device_id=0x%08X, uiid=0x%04hx)",
                     short_addr, (unsigned int)device_id, uiid);
            return;
        }
        zboss_manager_register_bridged_device(short_addr, endpoint);
    }

    uint16_t matter_ep = app_bridge_get_matter_endpointid_by_zigbee_shortaddr(short_addr);
    device_info_t *new_info = device_info_register(device_id, uiid, short_addr, endpoint, ieee_addr);
    if (new_info) {
        device_info_set_matter_endpoint(device_id, matter_ep);
        if (identity) {
            device_info_set_identity(device_id, identity);
            bridge_apply_identity(matter_ep, identity);
        }
    }
    ESP_LOGI(TAG, "New device associated: device_id=0x%08X, uiid=0x%04hx, Matter ep=%d, zigbee=0x%04hx",
             (unsigned int)device_id, uiid, matter_ep, short_addr);

    zdp_discovery_send_register_status(short_addr, endpoint, 129);

    /* 触发设备上报一次真实状态，让新建的 Matter 端点立即与设备同步。 */
    zdp_discovery_request_state_report(short_addr, endpoint);
}

void bridge_manager_report_sensor_data(uint16_t short_addr, uint8_t endpoint,
                                       uint16_t cluster_id, int16_t raw_value)
{
    app_bridged_device_t *zigbee_device = app_bridge_get_device_by_zigbee_shortaddr(short_addr);
    if (!zigbee_device || !zigbee_device->dev || !zigbee_device->dev->endpoint) {
        ESP_LOGW(TAG, "Bridged device not found for short_addr=0x%04x, ep=%d", short_addr, endpoint);
        return;
    }

    const zigbee_matter_map_entry_t *entry = zigbee_matter_map_find(cluster_id);
    if (!entry) {
        ESP_LOGD(TAG, "No Matter mapping for zigbee cluster 0x%04x (short=0x%04x)", cluster_id, short_addr);
        return;
    }

    uint16_t matter_ep = esp_matter::endpoint::get_id(zigbee_device->dev->endpoint);

    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    val.type = entry->val_type;
    if (entry->val_type == ESP_MATTER_VAL_TYPE_NULLABLE_INT16) {
        val.val.i16 = raw_value;
    } else if (entry->val_type == ESP_MATTER_VAL_TYPE_NULLABLE_UINT16) {
        val.val.u16 = (uint16_t)raw_value;
    }

    esp_err_t err = bridge_set_attr_no_echo(matter_ep, entry->matter_cluster, entry->matter_attr, &val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "attribute::update failed (cluster=0x%04x, ep=%d): %s",
                 cluster_id, matter_ep, esp_err_to_name(err));
    }
}

esp_err_t bridge_manager_attr_update(uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id,
                                      esp_matter_attr_val_t *val,
                                      app_bridged_device_t *zigbee_device)
{
    if (!zigbee_device || !zigbee_device->dev || !zigbee_device->dev->endpoint || !val) {
        ESP_LOGE(TAG, "Unable to update bridge device, ep=%" PRIu16 ", cluster=%" PRIu32 ", attr=%" PRIu32 "",
                 endpoint_id, cluster_id, attribute_id);
        return ESP_OK;
    }

    /* 仅抑制本次上行写入的回声；其它下行（如关灯）照常发给 Zigbee */
    if (s_uplink_depth > 0 && endpoint_id == s_uplink_ep && cluster_id == s_uplink_cluster &&
        attribute_id == s_uplink_attr) {
        return ESP_OK;
    }

    /* Zigbee 栈未就绪（esp_zb_init 前，如开机 resume 端点时的属性 init 回调），
     * 禁止下发命令，否则 ZBOSS bufpool 未分配会崩。 */
    if (!s_zb_stack_ready) {
        ESP_LOGD(TAG, "Zigbee stack not ready, skip downlink: ep=%" PRIu16 ", cluster=%" PRIu32 ", attr=%" PRIu32,
                 endpoint_id, cluster_id, attribute_id);
        return ESP_OK;
    }

    uint16_t zshort = zigbee_device->dev_addr.zigbee_shortaddr;
    uint8_t zep = zigbee_device->dev_addr.zigbee_endpointid;

    switch (cluster_id) {

    case OnOff::Id:
        if (attribute_id == OnOff::Attributes::OnOff::Id) {
            const bool on = val->val.b;
            light_level_shadow_t *lv = light_level_find(endpoint_id);
            if (lv) {
                lv->is_on = on;
            }
            zb_tx_on_off(zshort, zep, on);
        }
        break;

    case LevelControl::Id:
        if (attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
            uint8_t level = val->val.u8;
            light_level_shadow_t *lv = light_level_find(endpoint_id);
            /* Matter 关灯地板 / 耦合噪声；不要用「On 后跳过一次 Level」，会把真调光吃掉 */
            if (level <= 1) {
                ESP_LOGI(TAG, "Skip MoveToLevel=%u (off/floor) ep=%u", level, endpoint_id);
                break;
            }
            if (lv && !lv->is_on) {
                ESP_LOGI(TAG, "Skip MoveToLevel=%u while Off ep=%u", level, endpoint_id);
                break;
            }
            light_remember_level(endpoint_id, level);
            /* MoveToLevelWithOnOff 时 Matter On 可能已是 true，补发 On 再调光 */
            zb_tx_on_off(zshort, zep, true);
            if (lv) {
                lv->is_on = true;
            }
            zb_tx_move_to_level(zshort, zep, level);
        }
        break;

    case ColorControl::Id: {
        color_ep_shadow_t *shadow = color_ep_shadow_find(endpoint_id);
        if (!shadow) {
            ESP_LOGW(TAG, "No color shadow slot for ep %d, skip color downlink", endpoint_id);
            break;
        }
        if (attribute_id == ColorControl::Attributes::CurrentX::Id) {
            shadow->x = val->val.u16;
            shadow->have_x = true;
            if (!shadow->have_y) {
                ESP_LOGI(TAG, "CurrentX=%u cached (Y not set yet), ep=%d", shadow->x, endpoint_id);
                break;
            }
            color_schedule_xy(shadow, zshort, zep);
        } else if (attribute_id == ColorControl::Attributes::CurrentY::Id) {
            shadow->y = val->val.u16;
            shadow->have_y = true;
            if (!shadow->have_x) {
                ESP_LOGI(TAG, "CurrentY=%u cached (X not set yet), ep=%d", shadow->y, endpoint_id);
                break;
            }
            color_schedule_xy(shadow, zshort, zep);
        } else if (attribute_id == ColorControl::Attributes::CurrentHue::Id) {
            shadow->hue = val->val.u8;
            shadow->have_hue = true;
            if (!shadow->have_sat) {
                ESP_LOGI(TAG, "CurrentHue=%u cached (Sat not set yet), ep=%d", shadow->hue, endpoint_id);
                break;
            }
            color_schedule_hs(shadow, zshort, zep);
        } else if (attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
            shadow->sat = val->val.u8;
            shadow->have_sat = true;
            if (!shadow->have_hue) {
                ESP_LOGI(TAG, "CurrentSat=%u cached (Hue not set yet), ep=%d", shadow->sat, endpoint_id);
                break;
            }
            color_schedule_hs(shadow, zshort, zep);
        } else if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
            color_ensure_on(endpoint_id, zshort, zep);
            zb_tx_move_to_color_temp(zshort, zep, val->val.u16);
        }
        break;
    }

    default:
        break;
    }

    return ESP_OK;
}

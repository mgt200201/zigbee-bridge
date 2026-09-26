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

static const char *TAG = "bridge_manager";

using namespace chip::app::Clusters;
using namespace esp_matter;
using namespace esp_matter::cluster;

/** Aggregator 端点 ID */
static uint16_t s_aggregator_ep_id = chip::kInvalidEndpointId;

/* ---- 下行颜色配对影子状态（按 Matter 端点索引） ----
 * Matter 控制器一次只写一个颜色属性（如 CurrentX），而 Zigbee 命令需要成对值
 * (X+Y 或 H+S)。此影子表缓存配对值，凑齐后再发命令。 */
#define COLOR_EP_SHADOW_SLOTS 8
typedef struct {
    uint16_t matter_ep;
    bool valid;
    bool have_x, have_y;       /**< CurrentX / CurrentY 是否收到过 */
    bool have_hue, have_sat;   /**< CurrentHue / CurrentSaturation 是否收到过 */
    uint16_t x, y;
    uint8_t hue, sat;
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
            s_color_ep_shadow[i].matter_ep = matter_ep;
            s_color_ep_shadow[i].valid = true;
            s_color_ep_shadow[i].have_x = s_color_ep_shadow[i].have_y = false;
            s_color_ep_shadow[i].have_hue = s_color_ep_shadow[i].have_sat = false;
            s_color_ep_shadow[i].x = s_color_ep_shadow[i].y = 0;
            s_color_ep_shadow[i].hue = s_color_ep_shadow[i].sat = 0;
            return &s_color_ep_shadow[i];
        }
    }
    return NULL;
}

/* ---- 上行(上报)标志：Zigbee→Matter 上报更新 Matter 属性时，
       会触发 PRE_UPDATE(bridge_manager_attr_update) 把命令回发给 Zigbee，
       形成 "上报 → 又下发改回 " 的反馈环（日志里可见 MoveToLevel/MoveToColor 被反复回发）。
       置位后 bridge_manager_attr_update 提前返回，避免回发。 ---- */
static bool s_processing_uplink = false;

static esp_err_t bridge_set_attr_no_echo(uint16_t ep, uint32_t cluster, uint32_t attr,
                                         esp_matter_attr_val_t *val)
{
    s_processing_uplink = true;
    esp_err_t err = attribute::update(ep, cluster, attr, val);
    s_processing_uplink = false;
    return err;
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
    /* CurrentLevel 槽位是可空 uint8（Matter 规范中 LevelControl 为 nullable），
     * 往可空槽位写非可空值 esp_matter_uint8 会报 ESP_ERR_INVALID_ARG(258)。
     * 因此用可空类型 esp_matter_nullable_uint8 上报；
     * 并把 0xFF(255) 钳制为 254，避免触发 null 哨兵，否则 CurrentLevel 变 null 后，
     * HA 下发 OnOff/MoveToLevel 时 CopyCurrentLevel 读到 null 会拒绝，灯无法控制。 */
    uint8_t lv = (level == 0xFF) ? 0xFE : level;
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
        light_conf.level_control.on_level = static_cast<uint8_t>(254);
        esp_err_t err = endpoint::extended_color_light::add(ep, &light_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add extended_color_light: %s", esp_err_to_name(err));
            return err;
        }
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
        ESP_LOGE(TAG, "Unsupported bridged matter device type: 0x%04x", device_type_id);
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

/* Zigbee 栈就绪标志：esp_zb_init() 之前为 false。
 * 未就绪时禁止下发 Zigbee 命令（ZBOSS bufpool 尚未分配，调用会崩溃）。 */
static volatile bool s_zb_stack_ready = false;

void bridge_manager_set_zb_stack_ready(bool ready)
{
    s_zb_stack_ready = ready;
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

void bridge_manager_on_device_identified(uint32_t device_id, uint16_t uiid,
                                          uint16_t short_addr, uint8_t endpoint,
                                          const esp_zb_ieee_addr_t ieee_addr)
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

        if (info->uiid == UIID_UNKNOWN && uiid != UIID_UNKNOWN) {
            info->uiid = uiid;
        }

        ESP_LOGI(TAG, "Device reconnected: device_id=0x%08X, Matter ep=%d",
                 (unsigned int)device_id, info->matter_endpoint_id);
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
    }
    ESP_LOGI(TAG, "New device associated: device_id=0x%08X, uiid=0x%04hx, Matter ep=%d, zigbee=0x%04hx",
             (unsigned int)device_id, uiid, matter_ep, short_addr);

    zdp_discovery_send_register_status(short_addr, endpoint, 129);
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
    /* 【调试】确认 HA 命令路径是否真的触发本回调，以及 zigbee_device 是否为 NULL */
    ESP_LOGI(TAG, "[CB] attr_update: ep=%d cluster=0x%04x attr=0x%04x val_type=%d dev=%s",
             endpoint_id, cluster_id, attribute_id,
             val ? val->type : -1, zigbee_device ? "non-null" : "NULL");

    if (!zigbee_device || !zigbee_device->dev || !zigbee_device->dev->endpoint || !val) {
        ESP_LOGE(TAG, "Unable to update bridge device, ep=%" PRIu16 ", cluster=%" PRIu32 ", attr=%" PRIu32 "",
                 endpoint_id, cluster_id, attribute_id);
        return ESP_OK;
    }

    /* 上报(属性值由 Zigbee 上行写入)过程中触发本回调时，禁止回发命令，避免反馈环 */
    if (s_processing_uplink) {
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
            bool on = val->val.b;
            esp_zb_zcl_on_off_cmd_t cmd = {
                .zcl_basic_cmd = {
                    .dst_addr_u   = { .addr_short = zshort },
                    .dst_endpoint = zep,
                    .src_endpoint = HA_GW_ENDPOINT,
                },
                .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                .on_off_cmd_id = static_cast<uint8_t>(on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID),
            };
            uint8_t seq = esp_zb_zcl_on_off_cmd_req(&cmd);
            ESP_LOGI(TAG, "On/Off=%s sent to 0x%04x ep=%d (seq=%u)", on ? "ON" : "OFF", zshort, zep, seq);
        }
        break;

    case LevelControl::Id:
        if (attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
            esp_zb_zcl_move_to_level_cmd_t cmd = {
                .zcl_basic_cmd = {
                    .dst_addr_u   = { .addr_short = zshort },
                    .dst_endpoint = zep,
                    .src_endpoint = HA_GW_ENDPOINT,
                },
                .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                .level = val->val.u8,
                .transition_time = 0,
            };
            uint8_t seq = esp_zb_zcl_level_move_to_level_cmd_req(&cmd);
            ESP_LOGI(TAG, "MoveToLevel=%u sent to 0x%04x ep=%d (seq=%u)", cmd.level, zshort, zep, seq);
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
            esp_zb_zcl_color_move_to_color_cmd_t cmd = {
                .zcl_basic_cmd = {
                    .dst_addr_u   = { .addr_short = zshort },
                    .dst_endpoint = zep,
                    .src_endpoint = HA_GW_ENDPOINT,
                },
                .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                .color_x = shadow->x,
                .color_y = shadow->y,
                .transition_time = 0,
            };
            uint8_t seq = esp_zb_zcl_color_move_to_color_cmd_req(&cmd);
            ESP_LOGI(TAG, "MoveToColor(x=%u,y=%u) sent to 0x%04x ep=%d (seq=%u)",
                     cmd.color_x, cmd.color_y, zshort, zep, seq);
            /* 发送后清零配对标志：下一轮必须等全新的 X+Y，避免用上一轮残留的旧值
             * 先发出一条错误的中间命令，导致灯先闪到错误颜色再修正 */
            shadow->have_x = false;
            shadow->have_y = false;
        } else if (attribute_id == ColorControl::Attributes::CurrentY::Id) {
            shadow->y = val->val.u16;
            shadow->have_y = true;
            if (!shadow->have_x) {
                ESP_LOGI(TAG, "CurrentY=%u cached (X not set yet), ep=%d", shadow->y, endpoint_id);
                break;
            }
            esp_zb_zcl_color_move_to_color_cmd_t cmd = {
                .zcl_basic_cmd = {
                    .dst_addr_u   = { .addr_short = zshort },
                    .dst_endpoint = zep,
                    .src_endpoint = HA_GW_ENDPOINT,
                },
                .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                .color_x = shadow->x,
                .color_y = shadow->y,
                .transition_time = 0,
            };
            uint8_t seq = esp_zb_zcl_color_move_to_color_cmd_req(&cmd);
            ESP_LOGI(TAG, "MoveToColor(x=%u,y=%u) sent to 0x%04x ep=%d (seq=%u)",
                     cmd.color_x, cmd.color_y, zshort, zep, seq);
            shadow->have_x = false;
            shadow->have_y = false;
        } else if (attribute_id == ColorControl::Attributes::CurrentHue::Id) {
            shadow->hue = val->val.u8;
            shadow->have_hue = true;
            if (!shadow->have_sat) {
                ESP_LOGI(TAG, "CurrentHue=%u cached (Sat not set yet), ep=%d", shadow->hue, endpoint_id);
                break;
            }
            esp_zb_color_move_to_hue_saturation_cmd_t cmd = {
                .zcl_basic_cmd = {
                    .dst_addr_u   = { .addr_short = zshort },
                    .dst_endpoint = zep,
                    .src_endpoint = HA_GW_ENDPOINT,
                },
                .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                .hue = shadow->hue,
                .saturation = shadow->sat,
                .transition_time = 0,
            };
            uint8_t seq = esp_zb_zcl_color_move_to_hue_and_saturation_cmd_req(&cmd);
            ESP_LOGI(TAG, "MoveToHueSat(h=%u,s=%u) sent to 0x%04x ep=%d (seq=%u)",
                     cmd.hue, cmd.saturation, zshort, zep, seq);
            /* 发送后清零配对标志：下一轮必须等全新的 H+S，避免用上一轮残留的旧值
             * 先发出一条错误的中间命令，导致灯先闪到错误颜色再修正 */
            shadow->have_hue = false;
            shadow->have_sat = false;
        } else if (attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
            shadow->sat = val->val.u8;
            shadow->have_sat = true;
            if (!shadow->have_hue) {
                ESP_LOGI(TAG, "CurrentSat=%u cached (Hue not set yet), ep=%d", shadow->sat, endpoint_id);
                break;
            }
            esp_zb_color_move_to_hue_saturation_cmd_t cmd = {
                .zcl_basic_cmd = {
                    .dst_addr_u   = { .addr_short = zshort },
                    .dst_endpoint = zep,
                    .src_endpoint = HA_GW_ENDPOINT,
                },
                .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                .hue = shadow->hue,
                .saturation = shadow->sat,
                .transition_time = 0,
            };
            uint8_t seq = esp_zb_zcl_color_move_to_hue_and_saturation_cmd_req(&cmd);
            ESP_LOGI(TAG, "MoveToHueSat(h=%u,s=%u) sent to 0x%04x ep=%d (seq=%u)",
                     cmd.hue, cmd.saturation, zshort, zep, seq);
            shadow->have_hue = false;
            shadow->have_sat = false;
        } else if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
            /* 色温下走：HA 的 MoveToColorTemperature 命令会更新 ColorTemperatureMireds 属性，
             * 这里把它转发成 Zigbee 的 MoveToColorTemperature（color_temperature 单位同为 mireds） */
            esp_zb_zcl_color_move_to_color_temperature_cmd_t cmd = {
                .zcl_basic_cmd = {
                    .dst_addr_u   = { .addr_short = zshort },
                    .dst_endpoint = zep,
                    .src_endpoint = HA_GW_ENDPOINT,
                },
                .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
                .color_temperature = val->val.u16,
                .transition_time = 0,
            };
            uint8_t seq = esp_zb_zcl_color_move_to_color_temperature_cmd_req(&cmd);
            ESP_LOGI(TAG, "MoveToColorTemp(mireds=%u) sent to 0x%04x ep=%d (seq=%u)",
                     cmd.color_temperature, zshort, zep, seq);
        }
        break;
    }

    default:
        break;
    }

    return ESP_OK;
}
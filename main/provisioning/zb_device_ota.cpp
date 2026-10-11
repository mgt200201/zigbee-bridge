/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zb_device_ota.cpp
 *
 * @brief Zigbee 子设备 OTA Server + HA HTTP 上传
 *
 * 设计要点（可靠性优先）：
 *   1. 整包落 raw flash 分区，不走 SPIFFS/FAT，避免碎片与写放大。
 *   2. 解析标准 Zigbee OTA 文件头，拒绝非法包。
 *   3. 指定目标 IEEE 时强制 DEVICE_SPECIFIC，避免广播 Image Notify 误伤。
 *   4. Query 时校验 manuf/image_type，且优先匹配目标短地址。
 *   5. 同时只允许一路传输；next_data 从 flash 按需读，不占大堆。
 *   6. esp_zb_ota_upgrade_server_notify_req 在 Zigbee lock 下调用。
 */

#include "zb_device_ota.h"
#include "zboss_manager.h"
#include "device_info.h"

#include <esp_log.h>
#include <esp_check.h>
#include <esp_http_server.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <esp_zigbee_ota.h>
#include <zcl/esp_zigbee_zcl_ota.h>
#include <zcl/esp_zigbee_zcl_command.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "zb_dev_ota";

#define ZB_OTA_FILE_ID              0x0BEEF11Eu
#define ZB_OTA_MIN_HEADER_LEN       56
#define ZB_OTA_QUERY_JITTER         0x64
#define ZB_OTA_UPGRADE_DELAY_S      5
#define ZB_OTA_IMAGE_SLOT           0
#define ZB_OTA_BLOCK_BUF_SIZE       256
#define ZB_OTA_HTTP_RX_CHUNK        4096
/* 会话看门狗：避免失败后卡在 NOTIFYING/TRANSFERRING，只能靠断电恢复 */
#define ZB_OTA_NOTIFY_TIMEOUT_MS    45000   /* 发出 Image Notify 后等设备 Query 的上限 */
#define ZB_OTA_STALL_TIMEOUT_MS     30000   /* 传输中无数据请求（无进度）的上限 */
/* ZBOSS OTA Server 在回给客户端的 Query Next Image Response 里，会在我们传入的
 * image_size 之上再加一个它自己的 OTA 头开销再宣告出去：基础头 56B + 本网关强制
 * 开启的 DEVICE_SPECIFIC 目标 IEEE 8B = 64B。仅用于日志说明；切勿据此去改 image_size
 * (那会直接少下发 64B 数据，导致设备收不满整包而 incomplete 中止)。 */
#define ZB_OTA_SRV_HDR_BYTES        (56 + 8)

#pragma pack(push, 1)
typedef struct {
    uint32_t file_id;
    uint16_t header_version;
    uint16_t header_length;
    uint16_t field_control;
    uint16_t manufacturer_code;
    uint16_t image_type;
    uint32_t file_version;
    uint16_t stack_version;
    char     header_string[32];
    uint32_t total_image_size;
} zb_ota_file_header_raw_t;
#pragma pack(pop)

typedef struct {
    zb_device_ota_state_t state;
    const esp_partition_t *part;
    uint32_t image_len;           /**< 已写入/有效镜像字节数 */
    uint32_t read_offset;         /**< next_data 读偏移 */
    uint16_t manufacturer_code;
    uint16_t image_type;
    uint32_t file_version;
    uint16_t field_control;
    uint16_t header_length;
    esp_zb_ieee_addr_t target_ieee;
    bool     has_target;
    uint16_t target_short;        /**< 0xFFFF = 未知 */
    uint8_t  block_buf[ZB_OTA_BLOCK_BUF_SIZE];
    esp_timer_handle_t guard_timer;   /**< NOTIFYING/TRANSFERRING 会话看门狗 */
    char     last_error[96];
} zb_ota_ctx_t;

static zb_ota_ctx_t s_ctx = {
    .state = ZB_DEVICE_OTA_STATE_IDLE,
    .target_short = 0xFFFF,
};
static SemaphoreHandle_t s_mutex;
static httpd_handle_t s_httpd;
static bool s_httpd_started;

static void set_error(const char *msg)
{
    if (!msg) {
        s_ctx.last_error[0] = '\0';
        return;
    }
    snprintf(s_ctx.last_error, sizeof(s_ctx.last_error), "%s", msg);
}

static void lock_ctx(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void unlock_ctx(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

/* ---------------- 会话看门狗 ----------------
 * NOTIFYING（已发 Image Notify，等设备 Query）与 TRANSFERRING（下载中）都是"死状态"：
 * 一旦设备不再响应，状态会永久卡住，后续 stage_begin/start_upgrade 全被拒 400，
 * 只能靠网关断电重启（静态 s_ctx 重置）恢复。这里用一次性定时器兜底自动放行。
 * 必须在持有 ctx 锁的情况下调用。 */
static void guard_disarm_locked(void)
{
    if (s_ctx.guard_timer) {
        esp_timer_stop(s_ctx.guard_timer);
    }
}

static void guard_arm_locked(uint32_t ms)
{
    if (!s_ctx.guard_timer) {
        return;
    }
    esp_timer_stop(s_ctx.guard_timer);
    esp_timer_start_once(s_ctx.guard_timer, (uint64_t)ms * 1000);
}

static void guard_cb(void *arg)
{
    (void)arg;
    lock_ctx();
    if (s_ctx.state == ZB_DEVICE_OTA_STATE_NOTIFYING) {
        ESP_LOGW(TAG, "Image Notify got no client response; releasing session (retryable)");
        s_ctx.state = ZB_DEVICE_OTA_STATE_STAGED;
        set_error("notify timeout (retryable)");
    } else if (s_ctx.state == ZB_DEVICE_OTA_STATE_TRANSFERRING) {
        ESP_LOGW(TAG, "OTA transfer stalled at %lu/%lu; marking failed",
                 (unsigned long)s_ctx.read_offset, (unsigned long)s_ctx.image_len);
        s_ctx.state = ZB_DEVICE_OTA_STATE_FAILED;
        set_error("transfer stalled");
    }
    unlock_ctx();
}

/* ---------------- IEEE / query helpers ---------------- */

/** 解析 "AA:BB:CC:DD:EE:FF:11:22"（MSB 在前）→ little-endian ieee[0]=LSB */
static esp_err_t parse_ieee(const char *s, esp_zb_ieee_addr_t out)
{
    if (!s || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    unsigned bytes[8];
    int n = sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                   &bytes[0], &bytes[1], &bytes[2], &bytes[3],
                   &bytes[4], &bytes[5], &bytes[6], &bytes[7]);
    if (n != 8) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)bytes[7 - i];
    }
    return ESP_OK;
}

static void format_ieee(const esp_zb_ieee_addr_t ieee, char *buf, size_t buflen)
{
    if (!ieee || !buf || buflen < 24) {
        if (buf && buflen) {
            buf[0] = '\0';
        }
        return;
    }
    snprintf(buf, buflen, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             ieee[7], ieee[6], ieee[5], ieee[4],
             ieee[3], ieee[2], ieee[1], ieee[0]);
}

static esp_err_t resolve_target(const char *ieee_str, const char *short_str)
{
    s_ctx.has_target = false;
    s_ctx.target_short = 0xFFFF;
    memset(s_ctx.target_ieee, 0, sizeof(s_ctx.target_ieee));

    if (ieee_str && ieee_str[0]) {
        ESP_RETURN_ON_ERROR(parse_ieee(ieee_str, s_ctx.target_ieee), TAG, "bad ieee");
        s_ctx.has_target = true;
        device_info_t *info = device_info_find_by_ieee(s_ctx.target_ieee);
        if (info) {
            s_ctx.target_short = info->short_addr;
        } else {
            known_device_t *kd = zboss_manager_find_by_ieee(s_ctx.target_ieee);
            if (kd) {
                s_ctx.target_short = kd->short_addr;
            }
        }
        return ESP_OK;
    }

    if (short_str && short_str[0]) {
        unsigned v = 0;
        if (sscanf(short_str, "%i", (int *)&v) != 1 || v > 0xFFFF) {
            return ESP_ERR_INVALID_ARG;
        }
        s_ctx.target_short = (uint16_t)v;
        device_info_t *info = device_info_find_by_short_addr(s_ctx.target_short);
        if (info) {
            memcpy(s_ctx.target_ieee, info->ieee_addr, sizeof(esp_zb_ieee_addr_t));
            s_ctx.has_target = true;
        } else {
            known_device_t *kd = zboss_manager_find_by_short(s_ctx.target_short);
            if (kd) {
                memcpy(s_ctx.target_ieee, kd->ieee_addr, sizeof(esp_zb_ieee_addr_t));
                s_ctx.has_target = true;
            }
        }
        if (!s_ctx.has_target) {
            set_error("short addr not in device table");
            return ESP_ERR_NOT_FOUND;
        }
        return ESP_OK;
    }

    set_error("ieee or short required");
    return ESP_ERR_INVALID_ARG;
}

/* ---------------- Flash staging ---------------- */

static esp_err_t parse_and_validate_header(void)
{
    zb_ota_file_header_raw_t hdr = {};
    if (s_ctx.image_len < sizeof(hdr)) {
        set_error("image too small");
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(esp_partition_read(s_ctx.part, 0, &hdr, sizeof(hdr)), TAG, "read hdr");

    if (hdr.file_id != ZB_OTA_FILE_ID) {
        set_error("not a Zigbee OTA file (bad magic)");
        return ESP_ERR_INVALID_ARG;
    }
    if (hdr.header_length < ZB_OTA_MIN_HEADER_LEN || hdr.header_length > 256) {
        set_error("bad header length");
        return ESP_ERR_INVALID_ARG;
    }
    if (hdr.total_image_size == 0 || hdr.total_image_size > s_ctx.part->size) {
        set_error("bad total_image_size");
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_ctx.image_len != hdr.total_image_size) {
        ESP_LOGW(TAG, "uploaded %lu bytes, header total_image_size=%lu (using uploaded len)",
                 (unsigned long)s_ctx.image_len, (unsigned long)hdr.total_image_size);
        /* 仍用实际上传长度，避免头字段与文件不一致导致传空 */
        if (s_ctx.image_len < hdr.header_length) {
            set_error("upload shorter than OTA header");
            return ESP_ERR_INVALID_SIZE;
        }
    }

    s_ctx.manufacturer_code = hdr.manufacturer_code;
    s_ctx.image_type = hdr.image_type;
    s_ctx.file_version = hdr.file_version;
    s_ctx.field_control = hdr.field_control;
    s_ctx.header_length = hdr.header_length;

    ESP_LOGI(TAG, "OTA image OK: manuf=0x%04x type=0x%04x ver=0x%08lx size=%lu fc=0x%04x",
             s_ctx.manufacturer_code, s_ctx.image_type,
             (unsigned long)s_ctx.file_version, (unsigned long)s_ctx.image_len,
             s_ctx.field_control);
    return ESP_OK;
}

static esp_err_t stage_begin(void)
{
    if (!s_ctx.part) {
        set_error("no zb_ota partition");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_ctx.state == ZB_DEVICE_OTA_STATE_TRANSFERRING) {
        set_error("OTA transfer in progress");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.state == ZB_DEVICE_OTA_STATE_NOTIFYING) {
        /* 上一次 Image Notify 未被设备响应：视为放弃，允许重新上传 */
        ESP_LOGW(TAG, "superseding stale NOTIFYING session");
    }
    guard_disarm_locked();

    ESP_LOGI(TAG, "Erasing zb_ota partition (%lu bytes)...", (unsigned long)s_ctx.part->size);
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(s_ctx.part, 0, s_ctx.part->size), TAG, "erase");
    s_ctx.image_len = 0;
    s_ctx.read_offset = 0;
    s_ctx.state = ZB_DEVICE_OTA_STATE_IDLE;
    set_error(NULL);
    return ESP_OK;
}

static esp_err_t stage_write(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_OK;
    }
    if (s_ctx.image_len + len > s_ctx.part->size) {
        set_error("image exceeds zb_ota partition");
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_partition_write(s_ctx.part, s_ctx.image_len, data, len), TAG, "write");
    s_ctx.image_len += (uint32_t)len;
    return ESP_OK;
}

static esp_err_t stage_finish(void)
{
    ESP_RETURN_ON_ERROR(parse_and_validate_header(), TAG, "validate");
    s_ctx.state = ZB_DEVICE_OTA_STATE_STAGED;
    s_ctx.read_offset = 0;
    return ESP_OK;
}

/* ---------------- Zigbee OTA Server ---------------- */

static esp_err_t next_data_cb(esp_zb_ota_zcl_information_t message, uint16_t index,
                              uint8_t size, uint8_t **data)
{
    (void)message;
    (void)index;

    if (!data || size == 0 || size > ZB_OTA_BLOCK_BUF_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ctx.part || s_ctx.read_offset >= s_ctx.image_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t remain = s_ctx.image_len - s_ctx.read_offset;
    uint8_t to_read = size;
    if (to_read > remain) {
        to_read = (uint8_t)remain;
    }

    esp_err_t err = esp_partition_read(s_ctx.part, s_ctx.read_offset, s_ctx.block_buf, to_read);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "next_data read fail @%lu: %s",
                 (unsigned long)s_ctx.read_offset, esp_err_to_name(err));
        return err;
    }
    /* 不足 size 时仍返回缓冲；栈按实际镜像边界处理。补零避免未定义。 */
    if (to_read < size) {
        memset(s_ctx.block_buf + to_read, 0, size - to_read);
    }

    s_ctx.read_offset += to_read;
    *data = s_ctx.block_buf;
    /* 每次取数据都刷新停滞看门狗：设备停止请求数据即视为传输停滞 */
    guard_arm_locked(ZB_OTA_STALL_TIMEOUT_MS);

    if ((s_ctx.read_offset & 0xFFF) == 0 || s_ctx.read_offset >= s_ctx.image_len) {
        ESP_LOGI(TAG, "OTA send progress %lu/%lu",
                 (unsigned long)s_ctx.read_offset, (unsigned long)s_ctx.image_len);
    }
    return ESP_OK;
}

static esp_err_t notify_image_locked(bool notify_on)
{
    if (s_ctx.image_len == 0 ||
        (s_ctx.state != ZB_DEVICE_OTA_STATE_STAGED &&
         s_ctx.state != ZB_DEVICE_OTA_STATE_DONE &&
         s_ctx.state != ZB_DEVICE_OTA_STATE_FAILED)) {
        set_error("image not staged");
        return ESP_ERR_INVALID_STATE;
    }

    esp_zb_ota_upgrade_server_notify_req_t req = {};
    req.endpoint = HA_GW_ENDPOINT;
    req.index = ZB_OTA_IMAGE_SLOT;
    req.notify_on = notify_on ? 1 : 0;
    req.ota_upgrade_time = ZB_OTA_UPGRADE_DELAY_S;
    req.next_data_cb = next_data_cb;

    req.ota_file_header.manufacturer_code = s_ctx.manufacturer_code;
    req.ota_file_header.image_type = s_ctx.image_type;
    req.ota_file_header.file_version = s_ctx.file_version;
    req.ota_file_header.field_control = s_ctx.field_control;

    if (s_ctx.has_target) {
        req.ota_file_header.field_control |= ESP_ZB_ZCL_OTA_UPGRADE_FILE_HEADER_FC_DEVICE_SPECIFIC;
        memcpy(req.ota_file_header.optional.upgrade_file_destination,
               s_ctx.target_ieee, sizeof(esp_zb_ieee_addr_t));
    }

    /* image_size 必须 == 完整 OTA 文件字节数(含 56B 文件头 + SubElement + app)。
     * ZBOSS OTA Server 按该值逐块回调 next_data_cb 取数据，传入多少就只下发多少：
     * 若在此处少传(例如为了对齐宣告值而减 64)，设备会永远收不满整包而在结尾
     * "incomplete" 中止。
     * 注意 ZBOSS 在回给客户端的 Query Next Image Response 里会把这个值再加一个它
     * 自己的 OTA 头开销(基础 56B + 设备专属目标 IEEE 8B = 64B)宣告出去，因此抓包/
     * 子设备查询看到的大小会比真实文件多 64B；该偏差由 ZBOSS 产生、网关无法从这一侧
     * 消除，且设备端按 SubElement 声明的 app 长度校验，不受影响、可正常完成升级。 */
    req.ota_file_header.image_size = s_ctx.image_len;
    ESP_LOGI(TAG, "OTA register: file=%lu declared=%lu (ZBOSS announces +%d -> %lu)",
             (unsigned long)s_ctx.image_len,
             (unsigned long)req.ota_file_header.image_size,
             ZB_OTA_SRV_HDR_BYTES,
             (unsigned long)(req.ota_file_header.image_size + ZB_OTA_SRV_HDR_BYTES));

    s_ctx.read_offset = 0;
    esp_err_t err = esp_zb_ota_upgrade_server_notify_req(&req);
    if (err != ESP_OK) {
        set_error("notify_req failed");
        s_ctx.state = ZB_DEVICE_OTA_STATE_FAILED;
        ESP_LOGE(TAG, "esp_zb_ota_upgrade_server_notify_req: %s", esp_err_to_name(err));
        return err;
    }

    s_ctx.state = notify_on ? ZB_DEVICE_OTA_STATE_NOTIFYING : ZB_DEVICE_OTA_STATE_STAGED;
    if (notify_on) {
        guard_arm_locked(ZB_OTA_NOTIFY_TIMEOUT_MS);
    } else {
        guard_disarm_locked();
    }
    char ieee_s[32];
    format_ieee(s_ctx.target_ieee, ieee_s, sizeof(ieee_s));
    ESP_LOGI(TAG, "OTA image registered (notify=%d) target=%s short=0x%04x ver=0x%08lx",
             notify_on, s_ctx.has_target ? ieee_s : "(none)",
             s_ctx.target_short, (unsigned long)s_ctx.file_version);
    return ESP_OK;
}

static esp_err_t start_upgrade(void)
{
    if (s_ctx.image_len == 0) {
        set_error("image not staged");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.state == ZB_DEVICE_OTA_STATE_TRANSFERRING) {
        set_error("OTA transfer already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.state == ZB_DEVICE_OTA_STATE_NOTIFYING) {
        ESP_LOGW(TAG, "re-notifying (previous Image Notify not answered)");
    }
    if (!s_ctx.has_target) {
        set_error("target ieee/short required for reliable OTA");
        return ESP_ERR_INVALID_ARG;
    }

    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(5000))) {
        set_error("zigbee lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = notify_image_locked(true);
    esp_zb_lock_release();
    return err;
}

esp_err_t zb_device_ota_add_server_cluster(esp_zb_cluster_list_t *cluster_list)
{
    ESP_RETURN_ON_FALSE(cluster_list, ESP_ERR_INVALID_ARG, TAG, "null cluster_list");

    esp_zb_ota_cluster_cfg_t ota_cfg = {};
    ota_cfg.ota_upgrade_file_version = 0x1;
    ota_cfg.ota_upgrade_downloaded_file_ver = 0x1;
    ota_cfg.ota_upgrade_manufacturer = 0xFFFF;
    ota_cfg.ota_upgrade_image_type = 0xFFFF;

    esp_zb_attribute_list_t *ota_attr = esp_zb_ota_cluster_create(&ota_cfg);
    ESP_RETURN_ON_FALSE(ota_attr, ESP_ERR_NO_MEM, TAG, "ota_cluster_create");

    esp_zb_zcl_ota_upgrade_server_variable_t variable = {
        .query_jitter = ZB_OTA_QUERY_JITTER,
        .current_time = 0x12345,
        .file_count = 1,
    };
    ESP_RETURN_ON_ERROR(
        esp_zb_ota_cluster_add_attr(ota_attr, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_DATA_ID,
                                    (void *)&variable),
        TAG, "add SERVER_DATA");
    ESP_RETURN_ON_ERROR(
        esp_zb_cluster_list_add_ota_cluster(cluster_list, ota_attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE),
        TAG, "add ota cluster");

    ESP_LOGI(TAG, "OTA Upgrade Server cluster added on ep %d", HA_GW_ENDPOINT);
    return ESP_OK;
}

esp_err_t zb_device_ota_handle_action(esp_zb_core_action_callback_id_t callback_id,
                                     const void *message)
{
    if (!message) {
        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_OTA_UPGRADE_SRV_QUERY_IMAGE_CB_ID) {
        const auto *msg =
            static_cast<const esp_zb_zcl_ota_upgrade_server_query_image_message_t *>(message);
        if (msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "OTA query from 0x%04x: manuf=0x%04x type=0x%04x ver=0x%08lx",
                 msg->zcl_addr.u.short_addr, msg->manufacturer_code, msg->image_type,
                 (unsigned long)msg->version);

        lock_ctx();
        const bool staged = (s_ctx.state == ZB_DEVICE_OTA_STATE_STAGED ||
                             s_ctx.state == ZB_DEVICE_OTA_STATE_NOTIFYING ||
                             s_ctx.state == ZB_DEVICE_OTA_STATE_TRANSFERRING);
        const bool match = staged &&
            msg->manufacturer_code == s_ctx.manufacturer_code &&
            msg->image_type == s_ctx.image_type;
        const bool addr_ok = !s_ctx.has_target ||
            s_ctx.target_short == 0xFFFF ||
            msg->zcl_addr.u.short_addr == s_ctx.target_short;
        const uint32_t file_ver = s_ctx.file_version;
        unlock_ctx();

        if (!match) {
            ESP_LOGW(TAG, "OTA query rejected: no matching staged image");
            return ESP_ERR_NOT_FOUND;
        }
        if (!addr_ok) {
            ESP_LOGW(TAG, "OTA query rejected: not target device");
            return ESP_ERR_NOT_FOUND;
        }
        /* 客户端版本已 >= 镜像则拒绝；0xFFFFFFFF 表示未知，放行 */
        if (msg->version != 0xFFFFFFFFu && msg->version >= file_ver) {
            ESP_LOGI(TAG, "OTA query rejected: client already has ver>=0x%08lx",
                     (unsigned long)file_ver);
            return ESP_ERR_NOT_FOUND;
        }
        return ESP_OK;
    }

    if (callback_id == ESP_ZB_CORE_OTA_UPGRADE_SRV_STATUS_CB_ID) {
        const auto *msg =
            static_cast<const esp_zb_zcl_ota_upgrade_server_status_message_t *>(message);
        if (msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "OTA server status=%d client=0x%04x type=0x%04x ver=0x%08lx",
                 (int)msg->server_status, msg->zcl_addr.u.short_addr,
                 msg->image_type, (unsigned long)msg->version);

        lock_ctx();
        switch (msg->server_status) {
        case ESP_ZB_ZCL_OTA_UPGRADE_SERVER_STARTED:
            s_ctx.state = ZB_DEVICE_OTA_STATE_TRANSFERRING;
            s_ctx.read_offset = 0;
            guard_arm_locked(ZB_OTA_STALL_TIMEOUT_MS);
            break;
        case ESP_ZB_ZCL_OTA_UPGRADE_SERVER_END:
            s_ctx.state = ZB_DEVICE_OTA_STATE_DONE;
            set_error(NULL);
            guard_disarm_locked();
            break;
        case ESP_ZB_ZCL_OTA_UPGRADE_SERVER_ABORTED:
            s_ctx.state = ZB_DEVICE_OTA_STATE_FAILED;
            set_error("client aborted");
            guard_disarm_locked();
            break;
        default:
            break;
        }
        unlock_ctx();
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

zb_device_ota_state_t zb_device_ota_get_state(void)
{
    return s_ctx.state;
}

/* ---------------- HTTP API ---------------- */

static const char *state_str(zb_device_ota_state_t st)
{
    switch (st) {
    case ZB_DEVICE_OTA_STATE_IDLE:          return "idle";
    case ZB_DEVICE_OTA_STATE_STAGED:        return "staged";
    case ZB_DEVICE_OTA_STATE_NOTIFYING:     return "notifying";
    case ZB_DEVICE_OTA_STATE_TRANSFERRING:  return "transferring";
    case ZB_DEVICE_OTA_STATE_DONE:          return "done";
    case ZB_DEVICE_OTA_STATE_FAILED:        return "failed";
    default:                                return "unknown";
    }
}

static esp_err_t send_json(httpd_req_t *req, int http_status, const char *json)
{
    httpd_resp_set_status(req,
                          http_status == 200 ? HTTPD_200 :
                          http_status == 400 ? HTTPD_400 :
                          http_status == 404 ? HTTPD_404 :
                          http_status == 409 ? "409 Conflict" :
                          http_status == 500 ? HTTPD_500 : HTTPD_500);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static void build_status_json(char *buf, size_t buflen)
{
    char ieee_s[32] = "";
    if (s_ctx.has_target) {
        format_ieee(s_ctx.target_ieee, ieee_s, sizeof(ieee_s));
    }
    snprintf(buf, buflen,
             "{\"state\":\"%s\",\"image_len\":%lu,\"sent\":%lu,"
             "\"manuf\":\"0x%04X\",\"image_type\":\"0x%04X\",\"file_version\":\"0x%08lX\","
             "\"target_ieee\":\"%s\",\"target_short\":\"0x%04X\",\"error\":\"%s\"}",
             state_str(s_ctx.state),
             (unsigned long)s_ctx.image_len,
             (unsigned long)s_ctx.read_offset,
             s_ctx.manufacturer_code, s_ctx.image_type,
             (unsigned long)s_ctx.file_version,
             ieee_s, s_ctx.target_short,
             s_ctx.last_error);
}

static esp_err_t read_query(httpd_req_t *req, const char *key, char *out, size_t outlen)
{
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) {
        out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    char *q = (char *)malloc(qlen);
    if (!q) {
        return ESP_ERR_NO_MEM;
    }
    if (httpd_req_get_url_query_str(req, q, qlen) != ESP_OK) {
        free(q);
        return ESP_FAIL;
    }
    esp_err_t err = httpd_query_key_value(q, key, out, outlen);
    free(q);
    return err;
}

static esp_err_t http_recv_body_to_flash(httpd_req_t *req)
{
    int remaining = req->content_len;
    if (remaining <= 0) {
        set_error("empty body");
        return ESP_ERR_INVALID_SIZE;
    }
    if ((uint32_t)remaining > s_ctx.part->size) {
        set_error("body larger than zb_ota partition");
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(stage_begin(), TAG, "stage_begin");

    uint8_t *chunk = (uint8_t *)malloc(ZB_OTA_HTTP_RX_CHUNK);
    if (!chunk) {
        return ESP_ERR_NO_MEM;
    }

    while (remaining > 0) {
        int to_read = remaining > ZB_OTA_HTTP_RX_CHUNK ? ZB_OTA_HTTP_RX_CHUNK : remaining;
        int got = httpd_req_recv(req, (char *)chunk, to_read);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(chunk);
            set_error("http recv failed");
            s_ctx.state = ZB_DEVICE_OTA_STATE_FAILED;
            return ESP_FAIL;
        }
        esp_err_t err = stage_write(chunk, (size_t)got);
        if (err != ESP_OK) {
            free(chunk);
            s_ctx.state = ZB_DEVICE_OTA_STATE_FAILED;
            return err;
        }
        remaining -= got;
    }
    free(chunk);
    return stage_finish();
}

static esp_err_t handler_status(httpd_req_t *req)
{
    char json[384];
    lock_ctx();
    build_status_json(json, sizeof(json));
    unlock_ctx();
    return send_json(req, 200, json);
}

static esp_err_t handler_image_put(httpd_req_t *req)
{
    lock_ctx();
    esp_err_t err = http_recv_body_to_flash(req);
    char json[384];
    build_status_json(json, sizeof(json));
    unlock_ctx();
    return send_json(req, err == ESP_OK ? 200 : 400, json);
}

static esp_err_t handler_start(httpd_req_t *req)
{
    char ieee[40] = {};
    char short_s[16] = {};
    (void)read_query(req, "ieee", ieee, sizeof(ieee));
    (void)read_query(req, "short", short_s, sizeof(short_s));

    lock_ctx();
    esp_err_t err = resolve_target(ieee[0] ? ieee : NULL, short_s[0] ? short_s : NULL);
    if (err == ESP_OK) {
        err = start_upgrade();
    } else {
        s_ctx.state = ZB_DEVICE_OTA_STATE_FAILED;
    }
    char json[384];
    build_status_json(json, sizeof(json));
    unlock_ctx();
    return send_json(req, err == ESP_OK ? 200 : (err == ESP_ERR_NOT_FOUND ? 404 : 400), json);
}

static esp_err_t handler_upgrade(httpd_req_t *req)
{
    char ieee[40] = {};
    char short_s[16] = {};
    (void)read_query(req, "ieee", ieee, sizeof(ieee));
    (void)read_query(req, "short", short_s, sizeof(short_s));

    lock_ctx();
    esp_err_t err = resolve_target(ieee[0] ? ieee : NULL, short_s[0] ? short_s : NULL);
    if (err != ESP_OK) {
        s_ctx.state = ZB_DEVICE_OTA_STATE_FAILED;
        char json[384];
        build_status_json(json, sizeof(json));
        unlock_ctx();
        return send_json(req, err == ESP_ERR_NOT_FOUND ? 404 : 400, json);
    }

    err = http_recv_body_to_flash(req);
    if (err == ESP_OK) {
        err = start_upgrade();
    } else {
        s_ctx.state = ZB_DEVICE_OTA_STATE_FAILED;
    }
    char json[384];
    build_status_json(json, sizeof(json));
    unlock_ctx();
    return send_json(req, err == ESP_OK ? 200 : 400, json);
}

esp_err_t zb_device_ota_http_start(void)
{
    if (s_httpd_started) {
        return ESP_OK;
    }
    if (!s_ctx.part) {
        ESP_LOGW(TAG, "zb_ota partition missing, HTTP not started");
        return ESP_ERR_NOT_FOUND;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = ZB_DEVICE_OTA_HTTP_PORT;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;
    cfg.max_uri_handlers = 8;
    /* 大包上传：提高栈，避免在 handler 里擦分区时溢出 */
    cfg.stack_size = 8192;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/api/zb_ota/status",  .method = HTTP_GET,  .handler = handler_status,  .user_ctx = NULL },
        { .uri = "/api/zb_ota/image",   .method = HTTP_PUT,  .handler = handler_image_put,.user_ctx = NULL },
        { .uri = "/api/zb_ota/start",   .method = HTTP_POST, .handler = handler_start,   .user_ctx = NULL },
        { .uri = "/api/zb_ota/upgrade", .method = HTTP_PUT,  .handler = handler_upgrade, .user_ctx = NULL },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &uris[i]));
    }

    s_httpd_started = true;
    ESP_LOGI(TAG, "Zigbee device OTA HTTP on :%d  (PUT /api/zb_ota/upgrade?ieee=..)",
             ZB_DEVICE_OTA_HTTP_PORT);
    return ESP_OK;
}

esp_err_t zb_device_ota_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_mutex, ESP_ERR_NO_MEM, TAG, "mutex");
    }

    if (!s_ctx.guard_timer) {
        const esp_timer_create_args_t guard_args = {
            .callback = &guard_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "zb_ota_guard",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&guard_args, &s_ctx.guard_timer), TAG, "guard timer");
    }

    s_ctx.part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40,
                                          "zb_ota");
    if (!s_ctx.part) {
        /* 兼容：按 label 再找一次 */
        s_ctx.part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY,
                                              "zb_ota");
    }
    if (!s_ctx.part) {
        ESP_LOGE(TAG, "partition 'zb_ota' not found — check partitions.csv");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "zb_ota partition @ 0x%lx size %lu",
             (unsigned long)s_ctx.part->address, (unsigned long)s_ctx.part->size);
    return ESP_OK;
}

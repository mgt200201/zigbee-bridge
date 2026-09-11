/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "device_info.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>

static const char *TAG = "device_info";

/** NVS 命名空间 */
#define NVS_NAMESPACE "device_db"

/** NVS key 前缀 + 格式 */
#define NVS_KEY_UIID_PREFIX  "uiid_"
#define NVS_KEY_UIID_FMT     NVS_KEY_UIID_PREFIX "%08X"

/** 设备信息内存表 */
static device_info_t s_device_infos[MAX_DEVICE_INFO] = {0};

/* ==================== 内部工具函数 ==================== */

/**
 * @brief 根据 deviceId 生成 NVS key
 * @param device_id 设备ID
 * @param key 输出缓冲区 (至少 13 字节: "uiid_" + 8 hex + '\0')
 */
static void make_nvs_key(uint32_t device_id, char *key)
{
    sprintf(key, NVS_KEY_UIID_FMT, (unsigned int)device_id);
}

/**
 * @brief 查找空闲槽位
 * @return 空闲槽位索引，-1 表示已满
 */
static int find_free_slot(void)
{
    for (int i = 0; i < MAX_DEVICE_INFO; i++) {
        if (!s_device_infos[i].in_use) {
            return i;
        }
    }
    return -1;
}

/* ==================== 公开 API ==================== */

void device_info_init(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "NVS namespace '%s' not found, no persisted devices", NVS_NAMESPACE);
        } else {
            ESP_LOGW(TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(err));
        }
        return;
    }

    /* 遍历所有可能的 key，查找 uiid_xxxx 模式的条目 */
    int restored_count = 0;
    nvs_iterator_t it = NULL;
    esp_err_t iter_err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NAMESPACE, NVS_TYPE_STR, &it);
    while (iter_err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, NVS_KEY_UIID_PREFIX, strlen(NVS_KEY_UIID_PREFIX)) == 0) {
            /* 解析 deviceId 从 key 名中 */
            uint32_t device_id;
            unsigned int tmp_id;
            if (sscanf(info.key + strlen(NVS_KEY_UIID_PREFIX), "%08X", &tmp_id) == 1) {
                device_id = tmp_id;
                char uiid[MAX_UIID_LEN] = {0};
                size_t len = sizeof(uiid);
                err = nvs_get_str(handle, info.key, uiid, &len);
                if (err == ESP_OK) {
                    int slot = find_free_slot();
                    if (slot >= 0) {
                        device_info_t *dev = &s_device_infos[slot];
                        dev->in_use = true;
                        dev->device_id = device_id;
                        dev->online = false;  /* 初始离线，等待设备入网 */
                        dev->short_addr = 0xFFFF;
                        dev->zigbee_endpoint = 0;
                        dev->matter_endpoint_id = 0;
                        memset(dev->ieee_addr, 0, sizeof(esp_zb_ieee_addr_t));
                        dev->manufacturer[0] = '\0';
                        dev->model[0] = '\0';
                        dev->sw_version[0] = '\0';
                        strncpy(dev->uiid, uiid, MAX_UIID_LEN - 1);
                        dev->uiid[MAX_UIID_LEN - 1] = '\0';
                        restored_count++;
                        ESP_LOGI(TAG, "Restored device: device_id=0x%08X, uiid=%.32s", (unsigned int)device_id, uiid);
                    } else {
                        ESP_LOGW(TAG, "Device info table full, cannot restore device_id=0x%08X", (unsigned int)device_id);
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to read NVS key '%s': %s", info.key, esp_err_to_name(err));
                }
            }
        }
        iter_err = nvs_entry_next(&it);
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Restored %d devices from NVS", restored_count);
}

device_info_t *device_info_find_by_device_id(uint32_t device_id)
{
    if (device_id == DEVICE_ID_UNKNOWN) {
        return NULL;
    }
    for (int i = 0; i < MAX_DEVICE_INFO; i++) {
        if (s_device_infos[i].in_use && s_device_infos[i].device_id == device_id) {
            return &s_device_infos[i];
        }
    }
    return NULL;
}

device_info_t *device_info_find_by_short_addr(uint16_t short_addr)
{
    if (short_addr == 0xFFFF) {
        return NULL;
    }
    for (int i = 0; i < MAX_DEVICE_INFO; i++) {
        if (s_device_infos[i].in_use && s_device_infos[i].short_addr == short_addr) {
            return &s_device_infos[i];
        }
    }
    return NULL;
}

device_info_t *device_info_find_by_ieee(const esp_zb_ieee_addr_t ieee)
{
    for (int i = 0; i < MAX_DEVICE_INFO; i++) {
        if (s_device_infos[i].in_use &&
            memcmp(s_device_infos[i].ieee_addr, ieee, sizeof(esp_zb_ieee_addr_t)) == 0) {
            return &s_device_infos[i];
        }
    }
    return NULL;
}

device_info_t *device_info_register(uint32_t device_id, const char *uiid,
                                     uint16_t short_addr, uint8_t zigbee_endpoint,
                                     const esp_zb_ieee_addr_t ieee_addr)
{
    if (device_id == DEVICE_ID_UNKNOWN || device_id == 0) {
        ESP_LOGW(TAG, "Invalid device_id=0x%08X, denying registration", (unsigned int)device_id);
        return NULL;
    }

    /* 检查是否已存在（通过 deviceId） */
    device_info_t *existing = device_info_find_by_device_id(device_id);
    if (existing) {
        ESP_LOGI(TAG, "Device 0x%08X already registered, updating online status", (unsigned int)device_id);
        device_info_update_online(device_id, short_addr, zigbee_endpoint, ieee_addr);
        return existing;
    }

    /* 分配新槽位 */
    int slot = find_free_slot();
    if (slot < 0) {
        ESP_LOGE(TAG, "Device info table full, cannot register device_id=0x%08X", (unsigned int)device_id);
        return NULL;
    }

    device_info_t *dev = &s_device_infos[slot];
    dev->in_use = true;
    dev->device_id = device_id;
    dev->online = true;
    dev->short_addr = short_addr;
    dev->zigbee_endpoint = zigbee_endpoint;
    dev->matter_endpoint_id = 0;  /* 稍后由调用者设置 */
    memcpy(dev->ieee_addr, ieee_addr, sizeof(esp_zb_ieee_addr_t));
    dev->manufacturer[0] = '\0';
    dev->model[0] = '\0';
    dev->sw_version[0] = '\0';

    if (uiid) {
        strncpy(dev->uiid, uiid, MAX_UIID_LEN - 1);
        dev->uiid[MAX_UIID_LEN - 1] = '\0';
    } else {
        dev->uiid[0] = '\0';
    }

    /* 持久化到 NVS */
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        char key[16];
        make_nvs_key(device_id, key);
        err = nvs_set_str(handle, key, dev->uiid);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Persisted device: device_id=0x%08X, uiid=%.32s", (unsigned int)device_id, dev->uiid);
            } else {
                ESP_LOGW(TAG, "NVS commit failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "NVS set_str failed: %s", esp_err_to_name(err));
        }
        nvs_close(handle);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Registered new device: device_id=0x%08X, uiid=%s, short_addr=0x%04hx",
             (unsigned int)device_id, dev->uiid, short_addr);
    return dev;
}

void device_info_update_online(uint32_t device_id, uint16_t short_addr,
                                uint8_t zigbee_endpoint, const esp_zb_ieee_addr_t ieee_addr)
{
    device_info_t *dev = device_info_find_by_device_id(device_id);
    if (!dev) {
        ESP_LOGW(TAG, "Cannot update online: device_id=0x%08X not found", (unsigned int)device_id);
        return;
    }

    dev->online = true;
    if (short_addr != 0xFFFF) {
        dev->short_addr = short_addr;
    }
    if (zigbee_endpoint != 0) {
        dev->zigbee_endpoint = zigbee_endpoint;
    }
    if (ieee_addr) {
        memcpy(dev->ieee_addr, ieee_addr, sizeof(esp_zb_ieee_addr_t));
    }

    ESP_LOGI(TAG, "Device 0x%08X online: short_addr=0x%04hx, ep=%d",
             (unsigned int)device_id, dev->short_addr, dev->zigbee_endpoint);
}

void device_info_set_offline(uint16_t short_addr)
{
    device_info_t *dev = device_info_find_by_short_addr(short_addr);
    if (dev) {
        dev->online = false;
        ESP_LOGI(TAG, "Device 0x%08X (short_addr=0x%04hx) set offline",
                 (unsigned int)dev->device_id, short_addr);
    }
}

void device_info_set_matter_endpoint(uint32_t device_id, uint16_t matter_endpoint_id)
{
    device_info_t *dev = device_info_find_by_device_id(device_id);
    if (dev) {
        dev->matter_endpoint_id = matter_endpoint_id;
        ESP_LOGI(TAG, "Device 0x%08X -> Matter endpoint %d", (unsigned int)device_id, matter_endpoint_id);
    }
}

bool device_info_is_registered(uint32_t device_id)
{
    return device_info_find_by_device_id(device_id) != NULL;
}

int device_info_get_online_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_DEVICE_INFO; i++) {
        if (s_device_infos[i].in_use && s_device_infos[i].online) {
            count++;
        }
    }
    return count;
}

int device_info_get_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_DEVICE_INFO; i++) {
        if (s_device_infos[i].in_use) {
            count++;
        }
    }
    return count;
}

device_info_t *device_info_get_by_index(int index)
{
    int count = 0;
    for (int i = 0; i < MAX_DEVICE_INFO; i++) {
        if (s_device_infos[i].in_use) {
            if (count == index) {
                return &s_device_infos[i];
            }
            count++;
        }
    }
    return NULL;
}
/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file commissioning_mgr.cpp
 *
 * @brief 配网凭据管理实现
 *
 * NVS 键名：
 *   - "comm_pin"   → uint32_t, 配网 PIN 码
 *   - "comm_disc"  → uint16_t, Discriminator
 */

#include "commissioning_mgr.h"
#include <esp_err.h>
#include <esp_log.h>
#include <esp_random.h>
#include <inttypes.h>
#include <nvs_flash.h>
#include <nvs.h>

static const char *TAG = "comm_mgr";

#define NVS_NAMESPACE "commission"
#define NVS_KEY_PIN   "comm_pin"
#define NVS_KEY_DISC  "comm_disc"

static uint32_t s_pin = 0;
static uint16_t s_disc = 0;
static bool s_loaded = false;

static esp_err_t read_or_generate(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    /* 读取 PIN */
    err = nvs_get_u32(handle, NVS_KEY_PIN, &s_pin);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* 首次启动：生成随机 PIN（8 位，符合 Matter 规范） */
        s_pin = COMMISSIONING_PIN_MIN +
                (esp_random() % (COMMISSIONING_PIN_MAX - COMMISSIONING_PIN_MIN + 1));
        nvs_set_u32(handle, NVS_KEY_PIN, s_pin);
        nvs_commit(handle);
        ESP_LOGI(TAG, "Generated new PIN: %08" PRIu32, s_pin);
    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded PIN from NVS: %08" PRIu32, s_pin);
    } else {
        ESP_LOGW(TAG, "NVS read PIN failed: %s, using fallback", esp_err_to_name(err));
        s_pin = 20202021; /* 仅 NVS 损坏时的兜底 */
    }

    /* 读取 Discriminator */
    err = nvs_get_u16(handle, NVS_KEY_DISC, &s_disc);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_disc = (uint16_t)(esp_random() & 0x0FFF);
        nvs_set_u16(handle, NVS_KEY_DISC, s_disc);
        nvs_commit(handle);
        ESP_LOGI(TAG, "Generated new discriminator: %u (0x%03X)", s_disc, s_disc);
    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded discriminator from NVS: %u", s_disc);
    } else {
        s_disc = 3840; /* 兜底 */
    }

    nvs_close(handle);
    s_loaded = true;
    return ESP_OK;
}

esp_err_t commissioning_mgr_init(void)
{
    if (s_loaded) {
        return ESP_OK;
    }
    return read_or_generate();
}

uint32_t commissioning_mgr_get_pin(void)
{
    if (!s_loaded) {
        read_or_generate();
    }
    return s_pin;
}

uint16_t commissioning_mgr_get_discriminator(void)
{
    if (!s_loaded) {
        read_or_generate();
    }
    return s_disc;
}

esp_err_t commissioning_mgr_regenerate(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    s_pin = COMMISSIONING_PIN_MIN +
            (esp_random() % (COMMISSIONING_PIN_MAX - COMMISSIONING_PIN_MIN + 1));
    s_disc = (uint16_t)(esp_random() & 0x0FFF);

    nvs_set_u32(handle, NVS_KEY_PIN, s_pin);
    nvs_set_u16(handle, NVS_KEY_DISC, s_disc);
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Regenerated credentials: PIN=%08" PRIu32 ", disc=%u", s_pin, s_disc);
    return ESP_OK;
}
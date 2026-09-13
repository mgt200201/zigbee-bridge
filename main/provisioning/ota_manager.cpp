/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file ota_manager.cpp
 *
 * @brief OTA 固件升级管理实现
 */

#include "ota_manager.h"
#include "watchdog.h"
#include <esp_log.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <esp_app_desc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "ota_mgr";

static esp_https_ota_config_t s_ota_config = {0};
static esp_http_client_config_t s_http_config = {0};
static bool s_initialized = false;

static void ota_http_config_init(esp_http_client_config_t *cfg)
{
    *cfg = (esp_http_client_config_t){0};
    cfg->url = CONFIG_OTA_FW_UPGRADE_URL;
    cfg->timeout_ms = CONFIG_OTA_HTTP_TIMEOUT_MS;
    cfg->keep_alive_enable = true;
#ifdef CONFIG_OTA_HTTPS_CERT_PEM
    cfg->cert_pem = CONFIG_OTA_HTTPS_CERT_PEM;
#endif
}

esp_err_t ota_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ota_http_config_init(&s_http_config);
    s_ota_config = (esp_https_ota_config_t){
        .http_config = &s_http_config,
    };

#ifndef CONFIG_OTA_HTTPS_CERT_PEM
    ESP_LOGW(TAG, "No HTTPS cert configured, TLS verification disabled (INSECURE)");
#endif

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "OTA manager initialized (fw: %s, idf: %s)", app_desc->version, app_desc->idf_ver);
    ESP_LOGI(TAG, "OTA URL: %s", CONFIG_OTA_FW_UPGRADE_URL);

    s_initialized = true;
    return ESP_OK;
}

esp_err_t ota_manager_check(bool *new_version_available, char **server_version)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *new_version_available = false;
    *server_version = NULL;

    esp_http_client_config_t http_config;
    ota_http_config_init(&http_config);

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for version check");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_HEAD);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int content_len = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "Server response: %d, content-length: %d", status, content_len);
        if (status == 200 && content_len > 0) {
            *new_version_available = true;
        }
    } else {
        ESP_LOGW(TAG, "Version check HTTP failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

esp_err_t ota_manager_upgrade(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting OTA upgrade from %s", CONFIG_OTA_FW_UPGRADE_URL);

    watchdog_feed();

    ota_http_config_init(&s_http_config);

    esp_err_t ret = esp_https_ota(&s_ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA upgrade successful, restarting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    ESP_LOGE(TAG, "OTA upgrade failed: %s", esp_err_to_name(ret));
    return ret;
}

/* ==================== 周期性检查 ==================== */

#if CONFIG_OTA_AUTO_CHECK_HOURS > 0

static void ota_check_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Periodic OTA check triggered");
    bool new_available = false;
    char *server_ver = NULL;
    esp_err_t err = ota_manager_check(&new_available, &server_ver);
    if (err == ESP_OK && new_available) {
        ESP_LOGI(TAG, "New firmware available, triggering upgrade");
        ota_manager_upgrade();
    }
}

void ota_manager_start_periodic_check(void)
{
    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {
        .callback = &ota_check_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_check",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, CONFIG_OTA_AUTO_CHECK_HOURS * 3600ULL * 1000000ULL);
        ESP_LOGI(TAG, "Auto OTA check every %d hours", CONFIG_OTA_AUTO_CHECK_HOURS);
    } else {
        ESP_LOGW(TAG, "Failed to create OTA check timer");
    }
}

#else

void ota_manager_start_periodic_check(void)
{
    ESP_LOGI(TAG, "Auto OTA check disabled (CONFIG_OTA_AUTO_CHECK_HOURS=0)");
}

#endif
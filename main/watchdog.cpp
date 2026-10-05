/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "watchdog.h"
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>

static const char *TAG = "wdt";

static bool s_initialized = false;

esp_err_t watchdog_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    const esp_task_wdt_config_t config = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };

    esp_err_t err = esp_task_wdt_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        /* ESP-IDF v5.4+ 在 startup 阶段自动初始化 TWDT，无需重复 */
        ESP_LOGI(TAG, "TWDT already initialized by ESP-IDF, using existing config");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Init failed: %s", esp_err_to_name(err));
        return err;
    } else {
        ESP_LOGI(TAG, "Initialized (timeout=%" PRIu32 " s, panic=true)",
                 CONFIG_ESP_TASK_WDT_TIMEOUT_S);
    }

    /* 订阅当前任务（即 app_main 所在任务） */
    err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Add current task failed: %s", esp_err_to_name(err));
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t watchdog_add_self(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    esp_err_t err = esp_task_wdt_add(self);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Add task '%s' failed: %s",
                 pcTaskGetName(self), esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "Task '%s' added to watchdog", pcTaskGetName(self));
    }
    return err;
}

esp_err_t watchdog_remove_self(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    esp_err_t err = esp_task_wdt_delete(self);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Remove task '%s' failed: %s",
                 pcTaskGetName(self), esp_err_to_name(err));
    }
    return err;
}

esp_err_t watchdog_feed(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Reset only if THIS task is subscribed; otherwise IDF logs "task not found". */
    if (esp_task_wdt_status(NULL) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_task_wdt_reset();
}

bool watchdog_pause_self(void)
{
    if (!s_initialized) {
        return false;
    }
    if (esp_task_wdt_status(NULL) != ESP_OK) {
        return false;
    }
    (void)watchdog_remove_self();
    return true;
}

void watchdog_resume_self(bool paused)
{
    if (paused) {
        (void)watchdog_add_self();
    }
}
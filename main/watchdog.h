/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file watchdog.h
 *
 * @brief 任务看门狗（Task Watchdog Timer）
 *
 * ESP-IDF TWDT 机制：
 *   被监视的 FreeRTOS 任务必须在超时时间内 yield（让出 CPU），
 *   否则 IDLE hook 判定任务卡死 → 触发 panic + 重启。
 *
 * 使用方式：
 *   1. app_main 中调用 watchdog_init()
 *   2. 各长运行任务内部调用 watchdog_add_self()
 *   3. 长时间操作中周期性调用 watchdog_feed()（>10s 的操作）
 *
 * Kconfig：
 *   - CONFIG_ESP_TASK_WDT_TIMEOUT_S  →  超时时间（默认 10s）
 *   - CONFIG_ESP_TASK_WDT_PANIC      →  超时后 panic（否则仅警告）
 */

#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化任务看门狗并订阅当前任务
 *
 * 必须在 FreeRTOS 调度器启动后调用。
 * 超时时间由 CONFIG_ESP_TASK_WDT_TIMEOUT_S 决定。
 *
 * @return ESP_OK 成功
 */
esp_err_t watchdog_init(void);

/**
 * @brief 将当前任务加入看门狗监视列表
 *
 * 由各任务入口调用一次。调用后 IDLE hook 会周期性检查本任务是否 yield。
 * 注意：任务退出/删除前应调用 watchdog_remove_self()。
 *
 * @return ESP_OK 成功
 */
esp_err_t watchdog_add_self(void);

/**
 * @brief 将当前任务从看门狗监视列表移除
 *
 * 任务退出前调用，避免任务已删除但 TWDT 仍尝试检查。
 *
 * @return ESP_OK 成功
 */
esp_err_t watchdog_remove_self(void);

/**
 * @brief 喂狗（重置当前任务的超时计时器）
 *
 * 仅在以下情况需要手动调用：
 *   - 单次操作耗时 > TWDT 超时（例如 OTA 下载、Flash 擦写）
 *   - 注意：调用过频繁无副作用，不影响性能
 *
 * @return ESP_OK 成功
 */
esp_err_t watchdog_feed(void);

/**
 * 无法在阻塞等待中喂狗时（serial-flasher sync），先退订当前任务，结束后再订回去。
 * 返回退订前是否在 TWDT 上。
 */
bool watchdog_pause_self(void);
void watchdog_resume_self(bool paused);

#ifdef __cplusplus
}
#endif
/*
   SPDX-License-Identifier: Apache-2.0

   应用初始化编排接口。

   按启动阶段拆分初始化职责，app_main 只负责按顺序调用这些阶段函数：
     1. app_init_early           构建信息 / 自检 / 看门狗 / NVS / 配网凭据
     2. app_init_hardware        H2 保持运行 / 按键 / LED / 外部 RTC
     3. app_init_matter_stack    Matter 协议栈与桥接器装配
     4. app_init_runtime_services 桥接管理 / WiFi / OTA 服务
     5. app_init_zigbee_stack    Zigbee 协议栈与入网回调
     6. app_init_console         串口命令与按键事件路由
     7. app_init_finish          就绪日志与主任务收尾
*/

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 第 0 层：构建元信息、启动自检、看门狗、NVS、配网凭据同步 */
void app_init_early(void);

/** @brief 第 1 层：外设初始化（H2 保持运行、按键、两颗状态 LED、外部 RTC） */
void app_init_hardware(void);

/**
 * @brief 第 2 层：Matter 协议栈初始化
 * @return aggregator 端点 ID
 */
uint16_t app_init_matter_stack(void);

/** @brief 第 3 层：运行期服务初始化（桥接管理、WiFi、OTA） */
void app_init_runtime_services(uint16_t aggregator_endpoint_id);

/** @brief 第 4 层：Zigbee 协议栈启动与入网/LED 回调注册 */
void app_init_zigbee_stack(void);

/** @brief 第 5 层：串口命令任务与按键事件路由 */
void app_init_console(void);

/** @brief 收尾：打印就绪日志与堆信息，并把主任务从看门狗中移除 */
void app_init_finish(void);

#ifdef __cplusplus
}
#endif

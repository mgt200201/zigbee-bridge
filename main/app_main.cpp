/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file app_main.cpp
 *
 * @brief 应用入口——只做初始化顺序编排
 *
 * 本文件仅保留 app_main()，按启动阶段依次调用 app_init_*，不包含业务实现：
 *   - 阶段拆分与初始化实现 →  app_init.cpp
 *   - 状态 LED 状态机        →  peripherals/status_led.cpp
 *   - 外部 RTC 读数诊断      →  peripherals/rtc_time_diag.cpp
 *   - 串口命令               →  provisioning/serial_cmd.cpp
 *   - 配网凭据同步           →  provisioning/commissioning_sync.cpp
 *   - 制造商信息 Provider    →  matter/device_instance_provider.cpp
 *   - Matter/Chip 事件处理   →  matter/bridge_manager.cpp
 *   - Zigbee 网络管理        →  zigbee/zboss_manager.cpp
 *   - 外设驱动（按键/LED）   →  peripherals/button.cpp, led.cpp
 */

#include "app_init.h"

extern "C" void app_main(void)
{
    /* 第 0 层：构建信息、自检、看门狗、NVS、配网凭据 */
    app_init_early();

    /* 第 1 层：外设（H2 保持运行、按键、状态 LED、外部 RTC） */
    app_init_hardware();

    /* 第 2 层：Matter 协议栈与桥接器 */
    uint16_t aggregator_endpoint_id = app_init_matter_stack();

    /* 第 3 层：运行期服务（桥接管理、WiFi、OTA） */
    app_init_runtime_services(aggregator_endpoint_id);

    /* 第 4 层：Zigbee 协议栈 */
    app_init_zigbee_stack();

    /* 第 5 层：串口命令与按键事件 */
    app_init_console();

    /* 收尾：就绪日志、堆信息、退出主任务看门狗 */
    app_init_finish();
}

/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/**
 * @file zcl_bridge_api.h
 *
 * @brief 桥接层抽象接口（依赖注入）
 *
 * 大厂标准实践：设备处理器（handler）不直接调用全局函数 bridge_manager_report_*()，
 * 而是通过本接口注入的方式访问 Matter 桥接层。
 *
 * 这样做的好处：
 *   1. 可测试性 — 单测时可传入 mock 实现，不依赖 Matter 协议栈
 *   2. 解耦 — handler 只依赖接口定义，不依赖具体实现
 *   3. 可替换 — 未来 Matter 层换接口，只需修改一处注入代码
 *
 * 使用方式：
 *   bridge_manager_init() 中调用 zcl_bridge_api_register() 注入实现
 *   handler 中通过 zcl_bridge_api_get() 获取接口
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ==================== 桥接接口定义 ==================== */

typedef struct {

    /** 上报传感器数值（温度/湿度/气压等标量） */
    void (*report_sensor)(uint16_t short_addr, uint8_t endpoint,
                          uint16_t cluster_id, int16_t raw_value);

    /** 上报开关状态 */
    void (*report_on_off)(uint16_t short_addr, uint8_t endpoint, bool on);

    /* ---- 灯具相关上报 ---- */

    /** 上报亮度（0x0008 CurrentLevel，0-254） */
    void (*report_level)(uint16_t short_addr, uint8_t endpoint, uint8_t level);

    /** 上报颜色 XY（0x0300 CurrentX/CurrentY） */
    void (*report_color_xy)(uint16_t short_addr, uint8_t endpoint, uint16_t x, uint16_t y);

    /** 上报颜色 HSV（0x0300 CurrentHue/CurrentSaturation） */
    void (*report_color_hsv)(uint16_t short_addr, uint8_t endpoint, uint8_t hue, uint8_t sat);

    /** 上报色温（0x0300 ColorTemperature=0x0007, 单位 mireds） */
    void (*report_color_temperature)(uint16_t short_addr, uint8_t endpoint, uint16_t mireds);

    /** 查询设备是否已创建 Matter 桥接 */
    bool (*is_device_bridged)(uint16_t short_addr);

    /** 根据短地址获取 Zigbee 端点号 */
    uint8_t (*get_endpoint_by_short)(uint16_t short_addr);

} zcl_bridge_api_t;

/** ==================== API ==================== */

/**
 * @brief 注册桥接接口（初始化时由 bridge_manager 调用一次）
 *
 * @param api 接口实现指针（必须指向静态/全局变量，内部只存指针不拷贝）
 */
void zcl_bridge_api_register(const zcl_bridge_api_t *api);

/**
 * @brief 获取桥接接口（handler 内部使用）
 *
 * @return 已注册的接口指针，未注册时返回 NULL
 */
const zcl_bridge_api_t *zcl_bridge_api_get(void);

#ifdef __cplusplus
}
#endif
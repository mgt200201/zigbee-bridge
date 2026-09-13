/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <esp_zigbee_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @def MAX_DEVICE_INFO
 * @brief 最大支持的设备数量
 */
#define MAX_DEVICE_INFO        32

/**
 * @def ZB_BASIC_ATTR_DEVICE_ID
 * @brief Zigbee Basic 集群中设备 ID 属性的属性 ID
 */
#define ZB_BASIC_ATTR_DEVICE_ID    0x4001

/**
 * @def DEVICE_ID_UNKNOWN
 * @brief 无效的 deviceId 标记值
 */
#define DEVICE_ID_UNKNOWN      0xFFFFFFFF

/**
 * @def UIID_UNKNOWN
 * @brief 无效的 uiid 标记值
 */
#define UIID_UNKNOWN           0xFFFF

/**
 * @def MAX_NAME_LEN
 * @brief 制造商/型号字符串最大长度
 */
#define MAX_NAME_LEN           32

/**
 * @brief 设备信息结构体
 *
 * 存储一个 Zigbee 设备的核心信息，用于桥接到 Matter。
 * - deviceId 来自 Basic cluster 属性 0x4001
 * - uiid 根据 Model Identifier 映射得到（不读 0x4002）
 * - deviceId 和 uiid 会持久化到 NVS，网关重启后可恢复
 * - 制造商、型号、软件版本等非关键信息不持久化
 */
typedef struct {
    uint32_t device_id;                     /**< 设备ID (Basic 0x4001)，DEVICE_ID_UNKNOWN 表示未获取 */
    uint16_t uiid;                          /**< 设备UIID，根据型号映射得到，UIID_UNKNOWN 表示未知 */
    bool online;                            /**< 当前是否在线 */
    uint16_t short_addr;                    /**< 当前Zigbee短地址（可能随重加入网变化） */
    uint8_t zigbee_endpoint;                /**< Zigbee 端点号 */
    uint16_t matter_endpoint_id;            /**< Matter 桥接端点 ID */
    esp_zb_ieee_addr_t ieee_addr;           /**< IEEE 64位地址 (唯一标识硬件) */
    char manufacturer[MAX_NAME_LEN];        /**< 制造商名称（不持久化） */
    char model[MAX_NAME_LEN];               /**< 型号标识（不持久化） */
    char sw_version[16];                    /**< 软件版本（不持久化） */
    bool in_use;                            /**< 该条目是否有效 */
} device_info_t;

/**
 * @brief 初始化设备信息模块
 *
 * 从 NVS 中恢复已持久化的设备信息（deviceId -> uiid 映射）。
 * 需要在 nvs_flash_init() 之后、设备入网之前调用。
 */
void device_info_init(void);

/**
 * @brief 根据 deviceId 查找设备信息
 * @param device_id 设备ID
 * @return 找到返回设备指针，未找到返回 NULL
 */
device_info_t *device_info_find_by_device_id(uint32_t device_id);

/**
 * @brief 根据短地址查找设备信息
 * @param short_addr Zigbee 短地址
 * @return 找到返回设备指针，未找到返回 NULL
 */
device_info_t *device_info_find_by_short_addr(uint16_t short_addr);

/**
 * @brief 根据 IEEE 地址查找设备信息
 * @param ieee IEEE 64位地址
 * @return 找到返回设备指针，未找到返回 NULL
 */
device_info_t *device_info_find_by_ieee(const esp_zb_ieee_addr_t ieee);

/**
 * @brief 注册新设备
 *
 * 将设备信息添加到内存表中，并将 deviceId 和 uiid 持久化到 NVS。
 * 如果 deviceId 已存在，则更新在线状态和网络地址。
 *
 * @param device_id 设备ID (Basic 0x4001)
 * @param uiid 设备UIID（根据型号映射得到，为 0 表示未知）
 * @param short_addr 当前Zigbee短地址
 * @param zigbee_endpoint Zigbee端点号
 * @param ieee_addr IEEE 64位地址
 * @return 返回设备信息指针，失败返回 NULL
 */
device_info_t *device_info_register(uint32_t device_id, uint16_t uiid,
                                     uint16_t short_addr, uint8_t zigbee_endpoint,
                                     const esp_zb_ieee_addr_t ieee_addr);

/**
 * @brief 更新设备在线状态和网络地址
 *
 * 设备重入网时调用，更新短地址和在线状态。
 *
 * @param device_id 设备ID
 * @param short_addr 新的Zigbee短地址
 * @param zigbee_endpoint Zigbee端点号
 * @param ieee_addr IEEE 64位地址
 */
void device_info_update_online(uint32_t device_id, uint16_t short_addr,
                                uint8_t zigbee_endpoint, const esp_zb_ieee_addr_t ieee_addr);

/**
 * @brief 根据短地址设置设备离线
 * @param short_addr Zigbee 短地址
 */
void device_info_set_offline(uint16_t short_addr);

/**
 * @brief 彻底删除设备信息（包括 NVS 持久化数据）
 * @param device_id 设备ID
 */
void device_info_remove_by_device_id(uint32_t device_id);

/**
 * @brief 更新设备信息中的 Matter 桥接端点 ID
 * @param device_id 设备ID
 * @param matter_endpoint_id Matter 端点 ID
 */
void device_info_set_matter_endpoint(uint32_t device_id, uint16_t matter_endpoint_id);

/**
 * @brief 检查 deviceId 是否已注册（在 NVS 中有记录）
 * @param device_id 设备ID
 * @return true 已注册，false 未注册
 */
bool device_info_is_registered(uint32_t device_id);

/**
 * @brief 获取当前在线的设备数量
 * @return 在线设备数量
 */
int device_info_get_online_count(void);

/**
 * @brief 获取所有已注册设备数量
 * @return 设备总数
 */
int device_info_get_count(void);

/**
 * @brief 根据索引获取设备信息
 * @param index 索引（0 ~ get_count()-1）
 * @return 设备信息指针，越界返回 NULL
 */
device_info_t *device_info_get_by_index(int index);

/**
 * @brief 根据 Basic 集群 Model Identifier 属性值查找对应的 uiid
 *
 * 例如 model="TH-01" → uiid=0x0001。
 * 不通过读取 0x4002 属性，而是根据已知型号映射表查找。
 *
 * @param model 设备型号字符串（Basic 0x0005）
 * @return uint16_t uiid 值，未知型号返回 UIID_UNKNOWN (0xFFFF)
 */
uint16_t device_info_lookup_uiid(const char *model);

#ifdef __cplusplus
}
#endif
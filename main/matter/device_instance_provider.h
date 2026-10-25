/*
   SPDX-License-Identifier: Apache-2.0

   自定义 Device Instance Info Provider 注册接口。
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册自定义 Device Instance Info Provider
 *
 * 覆盖 Basic Information 集群的 VendorName，使网关暴露的设备制造商显示为
 * "XiaoNian"；其余设备信息沿用默认实现。必须在 esp_matter::start() 之前调用。
 */
void device_instance_provider_register(void);

#ifdef __cplusplus
}
#endif

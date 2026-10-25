/*
   SPDX-License-Identifier: Apache-2.0

   自定义 Device Instance Info Provider 实现。

   Matter Basic Information 集群的 VendorName(制造商) 读取走
   DeviceInstanceInfoProvider::GetVendorName()。这里覆盖它，让所有通过本网关
   暴露到 HA 的设备制造商都显示为 "XiaoNian"。其余设备信息沿用默认实现。
*/

#include "device_instance_provider.h"

#include <cstring>

#include <esp_log.h>

#include <esp_matter.h>
#include <platform/ESP32/ESP32Config.h>

#include <esp_matter_providers.h>
#include <platform/internal/GenericConfigurationManagerImpl.h>
#include <platform/internal/GenericDeviceInstanceInfoProvider.h>
#include <platform/internal/GenericDeviceInstanceInfoProvider.ipp>

static const char *TAG = "device_info_provider";

namespace {

using XiaoNianDeviceInfoBase =
    chip::DeviceLayer::Internal::GenericDeviceInstanceInfoProvider<chip::DeviceLayer::Internal::ESP32Config>;

using XiaoNianConfigMgrBase =
    chip::DeviceLayer::Internal::GenericConfigurationManagerImpl<chip::DeviceLayer::Internal::ESP32Config>;

class XiaoNianDeviceInstanceInfoProvider : public XiaoNianDeviceInfoBase
{
public:
    XiaoNianDeviceInstanceInfoProvider()
        : XiaoNianDeviceInfoBase(static_cast<XiaoNianConfigMgrBase &>(chip::DeviceLayer::ConfigurationMgr())) {}

    CHIP_ERROR GetVendorName(char *buf, size_t bufSize) override
    {
        constexpr char kXiaoNian[] = "XiaoNian";
        VerifyOrReturnError(bufSize >= sizeof(kXiaoNian), CHIP_ERROR_BUFFER_TOO_SMALL);
        memcpy(buf, kXiaoNian, sizeof(kXiaoNian));
        return CHIP_NO_ERROR;
    }
};

} /* namespace */

void device_instance_provider_register(void)
{
    static XiaoNianDeviceInstanceInfoProvider s_provider;
    esp_matter::set_custom_device_instance_info_provider(&s_provider);
    ESP_LOGI(TAG, "Manufacturer set to XiaoNian");
}

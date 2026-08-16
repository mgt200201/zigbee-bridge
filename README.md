# Zigbee Bridge

This example demonstrates a Matter-Zigbee Bridge that bridges Zigbee devices to Matter fabric.

The Matter Bridge device is composed of two parts: The RCP running on ESP32-H2 and the bridge app running on ESP32-S3.

See the [docs](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html) for more information about building and flashing the firmware.

💡 Important:  `create_bridge_devices` callback can be used to add data model elements (e.g., attributes, commands, etc.) to the bridge endpoint.

## 1. Additional Environment Setup

### 1.1 Hardware connection

There are two hardware type options for this example. You can choose one of the two options in menuconfig `ESP Matter Zigbee Bridge Example`->`Zigbee Bridge board type`.

#### 1.1.1 Standalone DevKit boards
Connect the two SoCs via UART, below is an example setup with ESP32-S3 DevKitC and ESP32-H2 DevKitC:

![Zigbee Bridge Hardware Connection](../../docs/_static/zigbee_bridge_hardware_connection.jpg)

|  ESP32-S3 Pin  | ESP32-H2 Pin |
|----------------|--------------|
|   GND          |    GND       |
|   GPIO4        |    TX        |
|   GPIO5        |    RX        |

#### 1.1.2 Zigbee Gateway DevKit board

![Zigbee Gateway DevKit Board](../../docs/_static/esp-thread-border-router-zigbee-gateway-board.png)

### 1.2 Build and flash the RCP (ESP32-H2)

Please use the [ot_rcp](https://github.com/espressif/esp-idf/tree/master/examples/openthread/ot_rcp) example to build the RCP for the ZigBee Bridge.

```
cd $IDF_PATH/examples/openthread/ot_rcp/
idf.py menuconfig -> OPENTHREAD_NCP_VENDOR_HOOK=y
idf.py set-target esp32h2
idf.py -p <port> build flash
```

**Note**: The two SoCs on the Zigbee Gateway DevKit board use USB ports while the standalone DevKit boards use UART ports.

### 1.3 Build and flash the Bridge (ESP32-S3)

For Standalone DevKit boards:

```
cd ${ESP_MATTER_PATH}/examples/zigbee_bridge
idf.py set-target esp32s3
idf.py -p <port> build flash
```

For Zigbee Gateway board:

```
cd ${ESP_MATTER_PATH}/examples/zigbee_bridge
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.zb_gw_board" set-target esp32s3 build
idf.py -p <port> flash
```

The Matter Zigbee Bridge will run on the ESP32-S3 and Zigbee network will be formed.

## 2. Post Commissioning Setup

### 2.1 Discovering Zigbee Devices

Commissioning the Matter Zigbee Bridge first, 0x7283 in description below is its Node Id.

You can read the PartsList from the Bridge to get the number of the bridged devices.

```
descriptor read parts-list 0x7283 0x0
```

If there is no other Zigbee device on the Zigbee Network, you will get a result with only an aggregator endpoint. Example:

```
Data = [
    1,  <---------------------------- Aggregator Endpoint
],
```

Then read the PartsList from the Aggregator Endpoint, you will get an empty result.

```
descriptor read parts-list 0x7283 0x1
...
Data = [

],
...
```

### 2.2 Setup Zigbee Bulb on ESP32-H2

Build and run Zigbee Bulb app on another ESP32-H2 board.

```
cd /path/to/esp-zigbee-sdk/examples/esp_zigbee_HA_sample/HA_on_off_light
idf.py set-target esp32h2
idf.py -p <port> build flash monitor
```

The Zigbee Bulb will be added to the Zigbee Network and a dynamic endpoint will be added on the Bridge device. You can read the PartsList on Aggregator Endpoint again to get the dynamic endpoint ID.

```
descriptor read parts-list 0x7283 0x1
```

The data will now contain the information of the connected Zigbee devices. Example:

```
Data = [
    2,  <--------------------------Bridged Endpoint
],
```

It means that the Zigbee Bulb is added as Endpoint 1 on the Zigbee Bridge. You can read the DeviceTypeList on the dynamic endpoint.

```
descriptor read device-type-list 0x7283 2
```

You will get the device types of the endpoint:

```
DeviceTypeList: 2 entries
  [1]: {
    Type: 19 <-------------------- Bridged Node device type
    Revision: 1
  }
  [2]: {
    Type: 256 <-------------------- OnOff Light device type
    Revision: 2
  }
```

You can also read the cluster ServerList on the dynamic endpoint.

```
descriptor read server-list 0x7283 0x1
```

This will give the list of supported server clusters. Example:

```
OnDescriptorServerListListAttributeResponse: 4 entries
    [1]: 3
    [2]: 4
    [3]: 5
    [4]: 6    <------------------------ OnOff Cluster
    [5]: 57   <------------------------ Bridged Device Basic Information Cluster
```

### 2.3 Control the bulb with chip-tool

Now you can control the Zigbee bulb using the chip tool.

```
onoff toggle 0x7283 0x2
```

## 3. Device Performance

### 3.1 Memory usage

The following is the Memory and Flash Usage.

-   `Bootup` == Device just finished booting up. Device is not
    commissioned or connected to wifi yet.
-   `After Commissioning` == Device is connected to wifi and is also
    commissioned and is rebooted.
-   `After Adding a Bridged device` == A Zigbee OnOff Light is added
    on the Bridge.
-   device used: esp32_devkit_c
-   tested on:
    [b40bf8e3](https://github.com/espressif/esp-matter/commit/b40bf8e398161bcac515fd57eb13d14e031e3a91)
    (2023-04-17)
-   IDF: release/v5.1 [420ebd20](https://github.com/espressif/esp-idf/commit/420ebd208ae9eb71cb71ebd22742d1175f11addc)

|                         | Bootup | After Commissioning | After Adding a Bridged device |
|:-                       |:-:     |:-:                  |:-:                            |
|**Free Internal Memory** |40KB    |113KB                |109KB                          |

**Flash Usage**: Firmware binary size: 1.6MB

This should give you a good idea about the amount of free memory that is
available for you to run your application's code.

Applications that do not require BLE post commissioning, So BLE is disable
once commissioning is complete in the test.
































mgt@mgt-virtual-machine:~/pro_esp/zigbee_matter_bridge$ idf.py build
Executing action: all (aliases: build)
Running ninja in directory /home/mgt/pro_esp/zigbee_matter_bridge/build
Executing "ninja all"...
[1/1] cd /home/mgt/pro_esp/zigbee_matter_bridge/bui...igbee_matter_bridge/build/bootloader/bootloader.bin
Bootloader binary size 0x5220 bytes. 0x2de0 bytes (36%) free.
[2/10] Performing build step for 'chip_gn'
ninja: no work to do.
[3/8] Building CXX object esp-idf/main/CMakeFiles/__idf_main.dir/app_zboss.cpp.obj
FAILED: esp-idf/main/CMakeFiles/__idf_main.dir/app_zboss.cpp.obj 
/home/mgt/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp32s3-elf-g++ -DCJSON_CIRCULAR_LIMIT=10000 -DCJSON_NESTING_LIMIT=1000 -DESP_MDNS_VERSION_NUMBER=\"1.11.3\" -DESP_PLATFORM -DIDF_VER=\"v5.4.1\" -DMBEDTLS_CONFIG_FILE=\"mbedtls/esp_config.h\" -DOPENTHREAD_CONFIG_FILE=\"openthread-core-esp32x-spinel-config.h\" -DOPENTHREAD_PROJECT_LIB_CONFIG_FILE=\"openthread-core-esp32x-spinel-config.h\" -DSOC_MMU_PAGE_SIZE=CONFIG_MMU_PAGE_SIZE -DSOC_XTAL_FREQ_MHZ=CONFIG_XTAL_FREQ -DUNITY_INCLUDE_CONFIG_H -D_GLIBCXX_HAVE_POSIX_SEMAPHORE -D_GLIBCXX_USE_POSIX_SEMAPHORE -D_GNU_SOURCE -D_POSIX_READER_WRITER_LOCKS -Dhollow -I/home/mgt/pro_esp/zigbee_matter_bridge/build/config -I/home/mgt/pro_esp/zigbee_matter_bridge/main -I/home/mgt/esp/esp-matter/examples/common/utils -I/home/mgt/esp/esp-idf-v5.4.1/components/newlib/platform_include -I/home/mgt/esp/esp-idf-v5.4.1/components/freertos/config/include -I/home/mgt/esp/esp-idf-v5.4.1/components/freertos/config/include/freertos -I/home/mgt/esp/esp-idf-v5.4.1/components/freertos/config/xtensa/include -I/home/mgt/esp/esp-idf-v5.4.1/components/freertos/FreeRTOS-Kernel/include -I/home/mgt/esp/esp-idf-v5.4.1/components/freertos/FreeRTOS-Kernel/portable/xtensa/include -I/home/mgt/esp/esp-idf-v5.4.1/components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos -I/home/mgt/esp/esp-idf-v5.4.1/components/freertos/esp_additions/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_hw_support/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_hw_support/include/soc -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_hw_support/include/soc/esp32s3 -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_hw_support/dma/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_hw_support/ldo/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_hw_support/debug_probe/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_hw_support/port/esp32s3/. -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_hw_support/port/esp32s3/include -I/home/mgt/esp/esp-idf-v5.4.1/components/heap/include -I/home/mgt/esp/esp-idf-v5.4.1/components/heap/tlsf -I/home/mgt/esp/esp-idf-v5.4.1/components/log/include -I/home/mgt/esp/esp-idf-v5.4.1/components/soc/include -I/home/mgt/esp/esp-idf-v5.4.1/components/soc/esp32s3 -I/home/mgt/esp/esp-idf-v5.4.1/components/soc/esp32s3/include -I/home/mgt/esp/esp-idf-v5.4.1/components/soc/esp32s3/register -I/home/mgt/esp/esp-idf-v5.4.1/components/hal/platform_port/include -I/home/mgt/esp/esp-idf-v5.4.1/components/hal/esp32s3/include -I/home/mgt/esp/esp-idf-v5.4.1/components/hal/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_rom/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_rom/esp32s3/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_rom/esp32s3/include/esp32s3 -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_rom/esp32s3 -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_common/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_system/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_system/port/soc -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_system/port/include/private -I/home/mgt/esp/esp-idf-v5.4.1/components/xtensa/esp32s3/include -I/home/mgt/esp/esp-idf-v5.4.1/components/xtensa/include -I/home/mgt/esp/esp-idf-v5.4.1/components/xtensa/deprecated_include -I/home/mgt/esp/esp-idf-v5.4.1/components/lwip/include -I/home/mgt/esp/esp-idf-v5.4.1/components/lwip/include/apps -I/home/mgt/esp/esp-idf-v5.4.1/components/lwip/include/apps/sntp -I/home/mgt/esp/esp-idf-v5.4.1/components/lwip/lwip/src/include -I/home/mgt/esp/esp-idf-v5.4.1/components/lwip/port/include -I/home/mgt/esp/esp-idf-v5.4.1/components/lwip/port/freertos/include -I/home/mgt/esp/esp-idf-v5.4.1/components/lwip/port/esp32xx/include -I/home/mgt/esp/esp-idf-v5.4.1/components/lwip/port/esp32xx/include/arch -I/home/mgt/esp/esp-idf-v5.4.1/components/lwip/port/esp32xx/include/sys -I/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__esp-zboss-lib/include -I/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__esp-zigbee-lib/include -I/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__esp-zigbee-lib/include/radio_spinel -I/home/mgt/esp/esp-idf-v5.4.1/components/driver/deprecated -I/home/mgt/esp/esp-idf-v5.4.1/components/driver/i2c/include -I/home/mgt/esp/esp-idf-v5.4.1/components/driver/touch_sensor/include -I/home/mgt/esp/esp-idf-v5.4.1/components/driver/twai/include -I/home/mgt/esp/esp-idf-v5.4.1/components/driver/touch_sensor/esp32s3/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_pm/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_ringbuf/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_gpio/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_pcnt/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_gptimer/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_spi/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_mcpwm/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_ana_cmpr/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_i2s/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_sdmmc/include -I/home/mgt/esp/esp-idf-v5.4.1/components/sdmmc/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_sdspi/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_sdio/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_dac/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_rmt/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_tsens/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_sdm/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_i2c/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_uart/include -I/home/mgt/esp/esp-idf-v5.4.1/components/vfs/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_ledc/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_parlio/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_usb_serial_jtag/include -I/home/mgt/esp/esp-idf-v5.4.1/components/ieee802154/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_coex/include -I/home/mgt/esp/esp-idf-v5.4.1/components/openthread/include -I/home/mgt/esp/esp-idf-v5.4.1/components/openthread/openthread/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_netif/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_event/include -I/home/mgt/esp/esp-idf-v5.4.1/components/mbedtls/port/include -I/home/mgt/esp/esp-idf-v5.4.1/components/mbedtls/mbedtls/include -I/home/mgt/esp/esp-idf-v5.4.1/components/mbedtls/mbedtls/library -I/home/mgt/esp/esp-idf-v5.4.1/components/mbedtls/esp_crt_bundle/include -I/home/mgt/esp/esp-idf-v5.4.1/components/mbedtls/mbedtls/3rdparty/everest/include -I/home/mgt/esp/esp-idf-v5.4.1/components/mbedtls/mbedtls/3rdparty/p256-m -I/home/mgt/esp/esp-idf-v5.4.1/components/mbedtls/mbedtls/3rdparty/p256-m/p256-m -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_app_format/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_bootloader_format/include -I/home/mgt/esp/esp-idf-v5.4.1/components/app_update/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bootloader_support/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bootloader_support/bootloader_flash/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_partition/include -I/home/mgt/esp/esp-idf-v5.4.1/components/efuse/include -I/home/mgt/esp/esp-idf-v5.4.1/components/efuse/esp32s3/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_mm/include -I/home/mgt/esp/esp-idf-v5.4.1/components/spi_flash/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_security/include -I/home/mgt/esp/esp-idf-v5.4.1/components/pthread/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_timer/include -I/home/mgt/esp/esp-idf-v5.4.1/components/app_trace/include -I/home/mgt/esp/esp-idf-v5.4.1/components/nvs_flash/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_phy/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_phy/esp32s3/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_vfs_console/include -I/home/mgt/esp/esp-idf-v5.4.1/components/wpa_supplicant/include -I/home/mgt/esp/esp-idf-v5.4.1/components/wpa_supplicant/port/include -I/home/mgt/esp/esp-idf-v5.4.1/components/wpa_supplicant/esp_supplicant/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_wifi/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_wifi/include/local -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_wifi/wifi_apps/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_wifi/wifi_apps/nan_app/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/include/esp32c3/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/common/osi/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/common/api/include/api -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/common/btc/profile/esp/blufi/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/common/btc/profile/esp/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/common/hci_log/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/common/ble_log/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/ans/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/bas/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/dis/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/gap/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/gatt/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/hr/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/htp/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/ias/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/ipss/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/lls/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/prox/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/cts/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/tps/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/hid/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/sps/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/services/cte/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/util/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/store/ram/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/host/store/config/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/porting/nimble/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/port/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/nimble/transport/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/porting/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/nimble/porting/npl/freertos/include -I/home/mgt/esp/esp-idf-v5.4.1/components/bt/host/nimble/esp-hci/include -I/home/mgt/esp/esp-idf-v5.4.1/components/unity/include -I/home/mgt/esp/esp-idf-v5.4.1/components/unity/unity/src -I/home/mgt/esp/esp-idf-v5.4.1/components/cmock/CMock/src -I/home/mgt/esp/esp-idf-v5.4.1/components/console -I/home/mgt/esp/esp-idf-v5.4.1/components/http_parser -I/home/mgt/esp/esp-idf-v5.4.1/components/esp-tls -I/home/mgt/esp/esp-idf-v5.4.1/components/esp-tls/esp-tls-crypto -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_adc/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_adc/interface -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_adc/esp32s3/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_adc/deprecated/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_isp/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_cam/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_cam/interface -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_jpeg/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_driver_ppa/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_eth/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_gdbstub/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_hid/include -I/home/mgt/esp/esp-idf-v5.4.1/components/tcp_transport/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_http_client/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_http_server/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_https_ota/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_https_server/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_psram/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_lcd/include -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_lcd/interface -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_lcd/rgb/include -I/home/mgt/esp/esp-idf-v5.4.1/components/protobuf-c/protobuf-c -I/home/mgt/esp/esp-idf-v5.4.1/components/protocomm/include/common -I/home/mgt/esp/esp-idf-v5.4.1/components/protocomm/include/security -I/home/mgt/esp/esp-idf-v5.4.1/components/protocomm/include/transports -I/home/mgt/esp/esp-idf-v5.4.1/components/protocomm/include/crypto/srp6a -I/home/mgt/esp/esp-idf-v5.4.1/components/protocomm/proto-c -I/home/mgt/esp/esp-idf-v5.4.1/components/esp_local_ctrl/include -I/home/mgt/esp/esp-idf-v5.4.1/components/espcoredump/include -I/home/mgt/esp/esp-idf-v5.4.1/components/espcoredump/include/port/xtensa -I/home/mgt/esp/esp-idf-v5.4.1/components/wear_levelling/include -I/home/mgt/esp/esp-idf-v5.4.1/components/fatfs/diskio -I/home/mgt/esp/esp-idf-v5.4.1/components/fatfs/src -I/home/mgt/esp/esp-idf-v5.4.1/components/fatfs/vfs -I/home/mgt/esp/esp-idf-v5.4.1/components/idf_test/include -I/home/mgt/esp/esp-idf-v5.4.1/components/idf_test/include/esp32s3 -I/home/mgt/esp/esp-idf-v5.4.1/components/json/cJSON -I/home/mgt/esp/esp-idf-v5.4.1/components/mqtt/esp-mqtt/include -I/home/mgt/esp/esp-idf-v5.4.1/components/nvs_sec_provider/include -I/home/mgt/esp/esp-idf-v5.4.1/components/perfmon/include -I/home/mgt/esp/esp-idf-v5.4.1/components/rt/include -I/home/mgt/esp/esp-idf-v5.4.1/components/spiffs/include -I/home/mgt/esp/esp-idf-v5.4.1/components/touch_element/include -I/home/mgt/esp/esp-idf-v5.4.1/components/usb/include -I/home/mgt/esp/esp-idf-v5.4.1/components/wifi_provisioning/include -I/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__esp_delta_ota/include -I/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__esp_secure_cert_mgr/include -I/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__esp_encrypted_img/include -I/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__mdns/include -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/src/platform/ESP32 -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/src/platform/OpenThread -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/src/include -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/src/lib -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/src -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/zzz_generated/app-common -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/examples/platform/esp32 -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/third_party/nlassert/repo/include -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/third_party/nlio/repo/include -I/home/mgt/pro_esp/zigbee_matter_bridge/build/esp-idf/chip/src/include -I/home/mgt/pro_esp/zigbee_matter_bridge/build/esp-idf/chip/include -I/home/mgt/pro_esp/zigbee_matter_bridge/build/esp-idf/chip/gen/include -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/config/esp32 -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/config/esp32/../.. -I/home/mgt/esp/esp-matter/components/esp_matter_console -I/home/mgt/esp/esp-matter/components/esp_matter -I/home/mgt/esp/esp-matter/components/esp_matter/utils -I/home/mgt/esp/esp-matter/components/esp_matter/data_model_provider -I/home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/third_party/nlfaultinjection/include -I/home/mgt/esp/esp-matter/components/esp_matter/zap_common -I/home/mgt/esp/esp-matter/components/esp_matter/data_model -I/home/mgt/esp/esp-matter/components/esp_matter_bridge -I/home/mgt/esp/esp-matter/examples/common/app_bridge -I/home/mgt/esp/esp-matter/device_hal/button_driver/button/include -I/home/mgt/esp/esp-matter/examples/common/app_reset -I/home/mgt/esp/esp-matter/components/esp_matter_rainmaker -I/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__led_strip/include -I/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__led_strip/interface -I/home/mgt/esp/esp-matter/device_hal/led_driver/include -I/home/mgt/esp/esp-matter/device_hal/device/include -I/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__cjson/cJSON -mlongcalls  -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-bzero -fno-builtin-stpcpy -fno-builtin-strncpy -ffunction-sections -fdata-sections -Wall -Werror=all -Wno-error=unused-function -Wno-error=unused-variable -Wno-error=unused-but-set-variable -Wno-error=deprecated-declarations -Wextra -Wno-error=extra -Wno-unused-parameter -Wno-sign-compare -Wno-enum-conversion -gdwarf-4 -ggdb -Og -fno-shrink-wrap -fmacro-prefix-map=/home/mgt/pro_esp/zigbee_matter_bridge=. -fmacro-prefix-map=/home/mgt/esp/esp-idf-v5.4.1=/IDF -fstrict-volatile-bitfields -fno-jump-tables -fno-tree-switch-conversion -Wno-error=uninitialized -Wno-error=maybe-uninitialized -Wno-missing-field-initializers -Wno-error=array-bounds -Wno-write-strings -Wno-format-nonliteral -Wno-format-security -Wno-error=cpp -std=gnu++2b -fno-exceptions -fno-rtti -fuse-cxa-atexit -std=gnu++17 -Os -DCHIP_HAVE_CONFIG_H "-DCHIP_ADDRESS_RESOLVE_IMPL_INCLUDE_HEADER=<lib/address_resolve/AddressResolve_DefaultImpl.h>" -MD -MT esp-idf/main/CMakeFiles/__idf_main.dir/app_zboss.cpp.obj -MF esp-idf/main/CMakeFiles/__idf_main.dir/app_zboss.cpp.obj.d -o esp-idf/main/CMakeFiles/__idf_main.dir/app_zboss.cpp.obj -c /home/mgt/pro_esp/zigbee_matter_bridge/main/app_zboss.cpp
/home/mgt/pro_esp/zigbee_matter_bridge/main/app_zboss.cpp: In function 'void zboss_task(void*)':
/home/mgt/pro_esp/zigbee_matter_bridge/main/app_zboss.cpp:154:31: warning: 'void esp_zb_main_loop_iteration()' is deprecated [-Wdeprecated-declarations]
  154 |     esp_zb_main_loop_iteration();
      |     ~~~~~~~~~~~~~~~~~~~~~~~~~~^~
In file included from /home/mgt/pro_esp/zigbee_matter_bridge/main/app_zboss.h:12,
                 from /home/mgt/pro_esp/zigbee_matter_bridge/main/app_zboss.cpp:9:
/home/mgt/pro_esp/zigbee_matter_bridge/managed_components/espressif__esp-zigbee-lib/include/esp_zigbee_core.h:386:6: note: declared here
  386 | void esp_zb_main_loop_iteration(void);
      |      ^~~~~~~~~~~~~~~~~~~~~~~~~~
/home/mgt/pro_esp/zigbee_matter_bridge/main/app_zboss.cpp: In function 'void button_timer_cb(void*)':
/home/mgt/pro_esp/zigbee_matter_bridge/main/app_zboss.cpp:207:63: error: void value not ignored as it ought to be
  207 |                         esp_err_t err = esp_zb_scheduler_alarm((esp_zb_callback_t)permit_join_scheduler_cb, 0, STEERING_DURATION_SEC);
      |                                         ~~~~~~~~~~~~~~~~~~~~~~^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ninja: build stopped: subcommand failed.
ninja failed with exit code 1, output of the command is in the /home/mgt/pro_esp/zigbee_matter_bridge/build/log/idf_py_stderr_output_115947 and /home/mgt/pro_esp/zigbee_matter_bridge/build/log/idf_py_stdout_output_115947


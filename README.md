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































load:0x403c8704,len:0xd20
load:0x403cb700,len:0x2f00
entry 0x403c8928
I (31) boot: ESP-IDF v5.4.1 2nd stage bootloader
I (31) boot: compile time Aug 15 2026 13:45:20
I (31) boot: Multicore bootloader
I (32) boot: chip revision: v0.2
I (34) boot: efuse block revision: v1.4
I (38) boot.esp32s3: Boot SPI Speed : 80MHz
I (42) boot.esp32s3: SPI Mode       : DIO
I (46) boot.esp32s3: SPI Flash Size : 4MB
I (49) boot: Enabling RNG early entropy source...
I (54) boot: Partition Table:
I (56) boot: ## Label            Usage          Type ST Offset   Length
I (63) boot:  0 esp_secure_cert  unknown          3f 06 0000d000 00002000
I (69) boot:  1 nvs              WiFi data        01 02 00010000 0000e000
I (76) boot:  2 nvs_keys         NVS keys         01 04 0001e000 00001000
I (82) boot:  3 otadata          OTA data         01 00 0001f000 00002000
I (89) boot:  4 phy_init         RF data          01 01 00021000 00001000
I (95) boot:  5 ota_0            OTA app          00 10 00030000 001d0000
I (102) boot:  6 ota_1            OTA app          00 11 00200000 001d0000
I (108) boot:  7 fctry            WiFi data        01 02 003d0000 00006000
I (115) boot:  8 zb_storage       Unknown data     01 81 003d6000 00020000
I (122) boot:  9 zb_fct           Unknown data     01 81 003f6000 00000400
I (128) boot: End of partition table
I (132) esp_image: segment 0: paddr=00030020 vaddr=3c160020 size=41284h (266884) map
I (186) esp_image: segment 1: paddr=000712ac vaddr=3fc9db00 size=05654h ( 22100) load
I (191) esp_image: segment 2: paddr=00076908 vaddr=40374000 size=09710h ( 38672) load
I (200) esp_image: segment 3: paddr=00080020 vaddr=42000020 size=15b680h (1422976) map
I (452) esp_image: segment 4: paddr=001db6a8 vaddr=4037d710 size=102e8h ( 66280) load
I (467) esp_image: segment 5: paddr=001eb998 vaddr=600fe100 size=0001ch (    28) load
I (477) boot: Loaded app from partition at offset 0x30000
I (477) boot: Disabling RNG early entropy source...
I (488) cpu_start: Multicore app
I (497) cpu_start: Pro cpu start user code
I (497) cpu_start: cpu freq: 160000000 Hz
I (497) app_init: Application information:
I (497) app_init: Project name:     zigbee_bridge
I (501) app_init: App version:      1.0
I (505) app_init: Compile time:     Aug 15 2026 13:45:16
I (510) app_init: ELF file SHA256:  5d244280b...
I (514) app_init: ESP-IDF:          v5.4.1
I (518) efuse_init: Min chip rev:     v0.0
I (522) efuse_init: Max chip rev:     v0.99 
I (526) efuse_init: Chip rev:         v0.2
I (530) heap_init: Initializing. RAM available for dynamic allocation:
I (536) heap_init: At 3FCC0EE8 len 00028828 (162 KiB): RAM
I (541) heap_init: At 3FCE9710 len 00005724 (21 KiB): RAM
I (546) heap_init: At 3FCF0000 len 00008000 (32 KiB): DRAM
I (551) heap_init: At 600FE11C len 00001ECC (7 KiB): RTCRAM
I (558) spi_flash: detected chip: boya
I (560) spi_flash: flash io: dio
W (563) spi_flash: Detected size(16384k) larger than the size in the binary image header(4096k). Using the size in the binary image header.
I (577) sleep_gpio: Configure to isolate all GPIO pins in sleep state
I (582) sleep_gpio: Enable automatic switching of GPIO sleep configuration
I (588) coexist: coex firmware version: e727207
I (607) coexist: coexist rom version e7ae62f
I (607) main_task: Started on CPU0
I (617) main_task: Calling app_main()
I (657) pp: pp rom version: e7ae62f
I (657) net80211: net80211 rom version: e7ae62f
I (667) wifi:wifi driver task: 3fcce4ec, prio:23, stack:6656, core=0
I (667) wifi:wifi firmware version: 79fa3f41ba
I (667) wifi:wifi certification version: v7.0
I (667) wifi:config NVS flash: enabled
I (667) wifi:config nano formatting: disabled
I (677) wifi:Init data frame dynamic rx buffer num: 32
I (677) wifi:Init static rx mgmt buffer num: 5
I (687) wifi:Init management short buffer num: 32
I (687) wifi:Init dynamic tx buffer num: 32
I (697) wifi:Init static tx FG buffer num: 2
I (697) wifi:Init static rx buffer size: 1600
I (697) wifi:Init static rx buffer num: 10
I (707) wifi:Init dynamic rx buffer num: 32
I (707) wifi_init: rx ba win: 6
I (707) wifi_init: accept mbox: 6
I (717) wifi_init: tcpip mbox: 32
I (717) wifi_init: udp mbox: 6
I (717) wifi_init: tcp mbox: 6
I (727) wifi_init: tcp tx win: 5760
I (727) wifi_init: tcp rx win: 5760
I (727) wifi_init: tcp mss: 1440
I (737) wifi_init: WiFi IRAM OP enabled
I (737) wifi_init: WiFi RX IRAM OP enabled
I (757) chip[DL]: NVS set: chip-counters/reboot-count = 559 (0x22F)
I (757) chip[DL]: Starting ESP WiFi layer
I (757) phy_init: phy_version 700,8582a7fd,Feb 10 2025,20:13:11
I (797) wifi:mode : sta (28:84:85:48:27:70) + softAP (28:84:85:48:27:71)
I (797) wifi:enable tsf
I (797) wifi:Total power save buffer number: 16
I (797) wifi:Init max length of beacon: 752/752
I (807) wifi:Init max length of beacon: 752/752
W (807) wifi:Haven't to connect to a suitable AP now!
I (817) chip[DL]: I (817) chip[DL]: Posting ESPSystemEvent: Wifi Event with eventId : 43Done driving station state, nothing else to do...

W (827) wifi:Haven't to connect to a suitable AP now!
I (827) chip[DL]: Done driving station state, nothing else to do...
I (827) chip[DL]: Posting ESPSystemEvent: Wifi Event with eventId : 2
I (837) chip[SVR]: I (847) chip[DL]: Initializing subscription resumption storage...
Posting ESPSystemEvent: Wifi Event with eventId : 43
I (847) chip[SVR]: I (857) chip[DL]: Server initializing...Posting ESPSystemEvent: Wifi Event with eventId : 12

I (867) chip[TS]: Last Known Good Time: 2023-10-14T01:16:48
I (867) chip[DMG]: AccessControl: initializing
I (877) chip[DMG]: Examples::AccessControlDelegate::Init
I (877) chip[DMG]: AccessControl: setting
I (887) chip[DMG]: DefaultAclStorage: initializing
I (887) chip[DMG]: DefaultAclStorage: 0 entries loaded
I (897) chip[SVR]: WARNING: mTestEventTriggerDelegate is null
I (947) chip[DMG]: AccessControlCluster: initializing
I (947) chip[DIS]: Updating services using commissioning mode 1
I (947) chip[DIS]: CHIP minimal mDNS started advertising.
I (947) chip[DIS]: Advertise commission parameter vendorID=65521 productID=32768 discriminator=3840/15 cm=1 cp=0 jf=0
I (957) chip[DIS]: CHIP minimal mDNS configured as 'Commissionable node device'; instance name: 4E9550A6B1B9FEEE.
I (967) chip[DIS]: mDNS service published: _matterc._udp
I (977) chip[IN]: CASE Server enabling CASE session setups
I (977) chip[SVR]: Joining Multicast groups
I (987) chip[SVR]: Server Listening...
I (987) chip[DL]: WIFI_EVENT_STA_START
E (987) app_bridged_device: Error reading the device type
W (987) wifi:Haven't to connect to a suitable AP now!
E (997) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 2
I (997) chip[DL]: Done driving station state, nothing else to do...
E (1007) app_bridged_device: Error reading the device type
I (1017) BLE_INIT: BT controller compile version [d74042a]
E (1027) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 3
I (1027) BLE_INIT: Bluetooth MAC: 28:84:85:48:27:72
E (1047) app_bridged_device: Error reading the device type
I (1047) NimBLE: GAP procedure initiated: stop advertising.

I (1057) CHIP[DL]: BLE host-controller synced
E (1057) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 4
E (1067) app_bridged_device: Error reading the device type
E (1077) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 5
E (1087) app_bridged_device: Error reading the device type
E (1087) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 6
E (1097) app_bridged_device: Error reading the device type
E (1107) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 7
E (1117) app_bridged_device: Error reading the device type
E (1117) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 8
E (1127) app_bridged_device: Error reading the device type
E (1137) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 9
E (1147) app_bridged_device: Error reading the device type
E (1147) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 10
E (1157) app_bridged_device: Error reading the device type
E (1167) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 11
I (1177) esp_matter_bridge: Cluster plugin init for the new added endpoint
I (1557) chip[DL]: Configuring CHIPoBLE advertising (interval 25 ms, connectable)
I (1557) NimBLE: GAP procedure initiated: advertise; 
I (1557) NimBLE: disc_mode=2
I (1557) NimBLE:  adv_channel_map=0 own_addr_type=1 adv_filter_policy=0 adv_itvl_min=40 adv_itvl_max=40
I (1567) NimBLE: 

I (1567) chip[DL]: CHIPoBLE advertising started
I (1577) app_main: Commissioning window opened
I (1577) chip[ZCL]: Boot reason: 0
I (1577) chip[ZCL]: Endpoint c On/off already set to new value
I (1587) esp_matter_bridge: Cluster plugin init for the new added endpoint
I (1597) chip[ZCL]: Boot reason: 0
I (1597) chip[ZCL]: Endpoint d On/off already set to new value
I (1607) esp_matter_bridge: Cluster plugin init for the new added endpoint
I (1607) chip[ZCL]: Boot reason: 0
I (1607) chip[ZCL]: Endpoint e On/off already set to new value
I (1617) esp_matter_bridge: Cluster plugin init for the new added endpoint
I (1627) chip[ZCL]: Boot reason: 0
I (1627) chip[ZCL]: Endpoint f On/off already set to new value
I (1697) gpio: GPIO[9]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 
I (1697) gpio: GPIO[10]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0 
I (1707) esp_zboss: Button/LED init done, GPIO9 initial level: 1
I (1707) ESP_RADIO_SPINEL: spinel UART interface initialization completed
I (1707) ESP_RADIO_SPINEL: Spinel UART interface has been successfully enabled
I (1727) ESP_ZIGBEE_RADIO_SPINEL_UART: Spinel UART interface enable successfully
                                                                                                                       I(1737) OPENTHREAD:I (1717) main_task: Returned from app_main()
[I] P-SpinelDrive-: co-processor reset: RESET_POWER_ON
E(1737) OPENTHREAD:[C] P-SpinelDrive-: Software reset co-processor successfully
> I (1857) esp_zboss: status: -1
I (1857) esp_zboss: Zigbee stack initialized
I (1877) esp_zboss: status: 0
I (1877) esp_zboss: Start network formation
I (1887) esp_zboss: Formed network successfully
I (1887) esp_zboss: ieee extended address: 74:4d:bd:ff:fe:6e:6a:46, PAN ID: 0x5b1a)
I (1897) esp_zboss: Network Key (for Wireshark decryption):
I (1897) esp_zboss: 1e 14 e6 3f a2 52 ce 65 f6 29 fe 0a 10 0f 36 1e
I (1907) esp_zboss: 短按 BOOT 按键可开放入网 180 秒
I (18927) esp_zboss: Button pressed (GPIO9=0), network_formed=1, steering_active=0
I (19087) esp_zboss: 短按触发 (160ms)！正在开放入网 180 秒...
I (19687) esp_zboss: status: 0
I (19687) esp_zboss: BDB steering 成功，设备已加入或网络已开放
I (19827) esp_zboss: status: 0
I (26997) esp_zboss: status: 0
I (27007) esp_zboss: status: 0
I (27047) esp_zboss: New device commissioned or rejoined (short: 0x625f)
I (27087) zigbee_bridge: on_off_light found: address:0x625f, endpoint:10, response_status:0
I (27097) esp_matter_bridge: Cluster plugin init for the new added endpoint
E (27097) esp_matter_bridge: Endpoints are used up
E (27107) chip[SVR]: Failed to unregister UserLabel on endpoint 28 - Error: d8

abort() was called at PC 0x4201cff3 on core 0
--- 0x4201cff3: chip::app::LazyRegisteredServerCluster<(anonymous namespace)::ESPMatterDescriptorCluster>::Destroy() at /home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/src/app/server-cluster/ServerClusterInterfaceRegistry.h:86


Backtrace: 0x40375c05:0x3fcf2640 0x40380f31:0x3fcf2660 0x4038897e:0x3fcf2680 0x4201cff3:0x3fcf26f0 0x4201d35b:0x3fcf2710 0x420125b5:0x3fcf2740 0x42074c7f:0x3fcf2770 0x42074fbd:0x3fcf2790 0x42034ebb:0x3fcf27c0 0x4200d830:0x3fcf2830 0x42035d21:0x3fcf2870 0x420466e0:0x3fcf2890 0x42061e6b:0x3fcf28d0 0x42035848:0x3fcf28f0 0x4200d29f:0x3fcf2910 0x40381991:0x3fcf2960
--- 0x40375c05: panic_abort at /home/mgt/esp/esp-idf-v5.4.1/components/esp_system/panic.c:454
--- 0x40380f31: esp_system_abort at /home/mgt/esp/esp-idf-v5.4.1/components/esp_system/port/esp_system_chip.c:87
--- 0x4038897e: abort at /home/mgt/esp/esp-idf-v5.4.1/components/newlib/abort.c:38
--- 0x4201cff3: chip::app::LazyRegisteredServerCluster<(anonymous namespace)::ESPMatterDescriptorCluster>::Destroy() at /home/mgt/esp/esp-matter/connectedhomeip/connectedhomeip/src/app/server-cluster/ServerClusterInterfaceRegistry.h:86
--- 0x4201d35b: ESPMatterDescriptorClusterServerShutdownCallback(unsigned short) at /home/mgt/esp/esp-matter/components/esp_matter/data_model_provider/clusters/descriptor_integration.cpp:122
--- 0x420125b5: esp_matter::endpoint::destroy(unsigned int*, unsigned int*) at /home/mgt/esp/esp-matter/components/esp_matter/data_model/esp_matter_data_model.cpp:1742
--- 0x42074c7f: esp_matter_bridge::remove_device(esp_matter_bridge::device*) at /home/mgt/esp/esp-matter/components/esp_matter_bridge/esp_matter_bridge.cpp:401
--- 0x42074fbd: esp_matter_bridge::create_device(unsigned int*, unsigned short, unsigned long, void*) at /home/mgt/esp/esp-matter/components/esp_matter_bridge/esp_matter_bridge.cpp:344
--- 0x42034ebb: app_bridge_create_bridged_device(unsigned int*, unsigned short, unsigned long, app_bridged_device_type_t, app_bridged_device_address_t, void*) at /home/mgt/esp/esp-matter/examples/common/app_bridge/app_bridged_device.cpp:181
--- 0x4200d830: zigbee_bridge_find_bridged_on_off_light_cb(esp_zb_zdp_status_t, unsigned short, unsigned char, void*) at /home/mgt/pro_esp/zigbee_matter_bridge/main/zigbee_bridge.cpp:40
--- 0x42035d21: find_device_cb at ??:?
--- 0x420466e0: zb_sched_loop_iteration at ??:?
--- 0x42061e6b: zboss_main_loop_iteration at ??:?
--- 0x42035848: esp_zb_main_loop_iteration at ??:?
--- 0x4200d29f: zboss_task(void*) at /home/mgt/pro_esp/zigbee_matter_bridge/main/app_zboss.cpp:193
--- 0x40381991: vPortTaskWrapper at /home/mgt/esp/esp-idf-v5.4.1/components/freertos/FreeRTOS-Kernel/portable/xtensa/port.c:139




ELF file SHA256: 5d244280b

Rebooting...
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
rst:0xc (RTC_SW_CPU_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)
Saved PC:0x40375b68
--- 0x40375b68: esp_restart_noos at /home/mgt/esp/esp-idf-v5.4.1/components/esp_system/port/soc/esp32s3/system_internal.c:160
SPIWP:0xee
mode:DIO, clock div:1
load:0x3fce2810,len:0x15a0
load:0x403c8700,len:0x4
load:0x403c8704,len:0xd20
load:0x403cb700,len:0x2f00
entry 0x403c8928
I (31) boot: ESP-IDF v5.4.1 2nd stage bootloader
I (31) boot: compile time Aug 15 2026 13:45:20
I (31) boot: Multicore bootloader
I (32) boot: chip revision: v0.2
I (34) boot: efuse block revision: v1.4
I (38) boot.esp32s3: Boot SPI Speed : 80MHz
I (42) boot.esp32s3: SPI Mode       : DIO
I (46) boot.esp32s3: SPI Flash Size : 4MB
I (49) boot: Enabling RNG early entropy source...
I (54) boot: Partition Table:
I (56) boot: ## Label            Usage          Type ST Offset   Length
I (63) boot:  0 esp_secure_cert  unknown          3f 06 0000d000 00002000
I (69) boot:  1 nvs              WiFi data        01 02 00010000 0000e000
I (76) boot:  2 nvs_keys         NVS keys         01 04 0001e000 00001000
I (82) boot:  3 otadata          OTA data         01 00 0001f000 00002000
I (89) boot:  4 phy_init         RF data          01 01 00021000 00001000
I (95) boot:  5 ota_0            OTA app          00 10 00030000 001d0000
I (102) boot:  6 ota_1            OTA app          00 11 00200000 001d0000
I (108) boot:  7 fctry            WiFi data        01 02 003d0000 00006000
I (115) boot:  8 zb_storage       Unknown data     01 81 003d6000 00020000
I (122) boot:  9 zb_fct           Unknown data     01 81 003f6000 00000400
I (128) boot: End of partition table
I (132) esp_image: segment 0: paddr=00030020 vaddr=3c160020 size=41284h (266884) map
I (186) esp_image: segment 1: paddr=000712ac vaddr=3fc9db00 size=05654h ( 22100) load
I (191) esp_image: segment 2: paddr=00076908 vaddr=40374000 size=09710h ( 38672) load
I (200) esp_image: segment 3: paddr=00080020 vaddr=42000020 size=15b680h (1422976) map
I (451) esp_image: segment 4: paddr=001db6a8 vaddr=4037d710 size=102e8h ( 66280) load
I (465) esp_image: segment 5: paddr=001eb998 vaddr=600fe100 size=0001ch (    28) load
I (476) boot: Loaded app from partition at offset 0x30000
I (476) boot: Disabling RNG early entropy source...
I (486) cpu_start: Multicore app
I (495) cpu_start: Pro cpu start user code
I (495) cpu_start: cpu freq: 160000000 Hz
I (495) app_init: Application information:
I (495) app_init: Project name:     zigbee_bridge
I (500) app_init: App version:      1.0
I (503) app_init: Compile time:     Aug 15 2026 13:45:16
I (508) app_init: ELF file SHA256:  5d244280b...
I (512) app_init: ESP-IDF:          v5.4.1
I (516) efuse_init: Min chip rev:     v0.0
I (520) efuse_init: Max chip rev:     v0.99 
I (524) efuse_init: Chip rev:         v0.2
I (528) heap_init: Initializing. RAM available for dynamic allocation:
I (534) heap_init: At 3FCC0EE8 len 00028828 (162 KiB): RAM
I (539) heap_init: At 3FCE9710 len 00005724 (21 KiB): RAM
I (544) heap_init: At 3FCF0000 len 00008000 (32 KiB): DRAM
I (550) heap_init: At 600FE11C len 00001ECC (7 KiB): RTCRAM
I (556) spi_flash: detected chip: boya
I (558) spi_flash: flash io: dio
W (561) spi_flash: Detected size(16384k) larger than the size in the binary image header(4096k). Using the size in the binary image header.
I (576) sleep_gpio: Configure to isolate all GPIO pins in sleep state
I (580) sleep_gpio: Enable automatic switching of GPIO sleep configuration
I (587) coexist: coex firmware version: e727207
I (605) coexist: coexist rom version e7ae62f
I (606) main_task: Started on CPU0
I (616) main_task: Calling app_main()
I (656) pp: pp rom version: e7ae62f
I (656) net80211: net80211 rom version: e7ae62f
I (666) wifi:wifi driver task: 3fcce4ec, prio:23, stack:6656, core=0
I (666) wifi:wifi firmware version: 79fa3f41ba
I (666) wifi:wifi certification version: v7.0
I (666) wifi:config NVS flash: enabled
I (666) wifi:config nano formatting: disabled
I (676) wifi:Init data frame dynamic rx buffer num: 32
I (676) wifi:Init static rx mgmt buffer num: 5
I (686) wifi:Init management short buffer num: 32
I (686) wifi:Init dynamic tx buffer num: 32
I (696) wifi:Init static tx FG buffer num: 2
I (696) wifi:Init static rx buffer size: 1600
I (696) wifi:Init static rx buffer num: 10
I (706) wifi:Init dynamic rx buffer num: 32
I (706) wifi_init: rx ba win: 6
I (706) wifi_init: accept mbox: 6
I (716) wifi_init: tcpip mbox: 32
I (716) wifi_init: udp mbox: 6
I (716) wifi_init: tcp mbox: 6
I (726) wifi_init: tcp tx win: 5760
I (726) wifi_init: tcp rx win: 5760
I (726) wifi_init: tcp mss: 1440
I (736) wifi_init: WiFi IRAM OP enabled
I (736) wifi_init: WiFi RX IRAM OP enabled
I (756) chip[DL]: NVS set: chip-counters/reboot-count = 560 (0x230)
I (756) chip[DL]: Starting ESP WiFi layer
I (756) phy_init: phy_version 700,8582a7fd,Feb 10 2025,20:13:11
I (796) wifi:mode : sta (28:84:85:48:27:70) + softAP (28:84:85:48:27:71)
I (796) wifi:enable tsf
I (796) wifi:Total power save buffer number: 16
I (796) wifi:Init max length of beacon: 752/752
I (806) wifi:Init max length of beacon: 752/752
W (806) wifi:Haven't to connect to a suitable AP now!
I (806) chip[DL]: I (806) chip[DL]: Posting ESPSystemEvent: Wifi Event with eventId : 43Done driving station state, nothing else to do...

W (826) wifi:Haven't to connect to a suitable AP now!
I (826) chip[DL]: Done driving station state, nothing else to do...
I (826) chip[DL]: Posting ESPSystemEvent: Wifi Event with eventId : 2
I (836) chip[SVR]: I (836) chip[DL]: Initializing subscription resumption storage...
Posting ESPSystemEvent: Wifi Event with eventId : 43
I (846) chip[SVR]: I (856) chip[DL]: Server initializing...Posting ESPSystemEvent: Wifi Event with eventId : 12

I (866) chip[TS]: Last Known Good Time: 2023-10-14T01:16:48
I (866) chip[DMG]: AccessControl: initializing
I (876) chip[DMG]: Examples::AccessControlDelegate::Init
I (876) chip[DMG]: AccessControl: setting
I (876) chip[DMG]: DefaultAclStorage: initializing
I (886) chip[DMG]: DefaultAclStorage: 0 entries loaded
I (886) chip[SVR]: WARNING: mTestEventTriggerDelegate is null
I (946) chip[DMG]: AccessControlCluster: initializing
I (946) chip[DIS]: Updating services using commissioning mode 1
I (946) chip[DIS]: CHIP minimal mDNS started advertising.
I (946) chip[DIS]: Advertise commission parameter vendorID=65521 productID=32768 discriminator=3840/15 cm=1 cp=0 jf=0
I (956) chip[DIS]: CHIP minimal mDNS configured as 'Commissionable node device'; instance name: FF50C6672BD2EAB6.
I (966) chip[DIS]: mDNS service published: _matterc._udp
I (976) chip[IN]: CASE Server enabling CASE session setups
I (976) chip[SVR]: Joining Multicast groups
I (986) chip[SVR]: Server Listening...
I (986) chip[DL]: WIFI_EVENT_STA_START
E (986) app_bridged_device: Error reading the device type
W (986) wifi:Haven't to connect to a suitable AP now!
E (996) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 2
I (996) chip[DL]: Done driving station state, nothing else to do...
E (1006) app_bridged_device: Error reading the device type
I (1016) BLE_INIT: BT controller compile version [d74042a]
E (1026) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 3
I (1026) BLE_INIT: Bluetooth MAC: 28:84:85:48:27:72
E (1046) app_bridged_device: Error reading the device type
I (1046) NimBLE: GAP procedure initiated: stop advertising.

I (1056) CHIP[DL]: BLE host-controller synced
E (1056) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 4
E (1066) app_bridged_device: Error reading the device type
E (1076) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 5
E (1086) app_bridged_device: Error reading the device type
E (1086) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 6
E (1096) app_bridged_device: Error reading the device type
E (1106) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 7
E (1116) app_bridged_device: Error reading the device type
E (1116) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 8
E (1126) app_bridged_device: Error reading the device type
E (1136) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 9
E (1146) app_bridged_device: Error reading the device type
E (1146) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 10
E (1156) app_bridged_device: Error reading the device type
E (1166) app_bridged_device: Failed to read the app_bridged_device_type and app_bridged_device_address for endpoint 11
I (1176) esp_matter_bridge: Cluster plugin init for the new added endpoint
I (1556) chip[DL]: Configuring CHIPoBLE advertising (interval 25 ms, connectable)
I (1556) NimBLE: GAP procedure initiated: advertise; 
I (1556) NimBLE: disc_mode=2
I (1556) NimBLE:  adv_channel_map=0 own_addr_type=1 adv_filter_policy=0 adv_itvl_min=40 adv_itvl_max=40
I (1566) NimBLE: 

I (1566) chip[DL]: CHIPoBLE advertising started
I (1576) app_main: Commissioning window opened
I (1576) chip[ZCL]: Boot reason: 0
I (1576) chip[ZCL]: Endpoint c On/off already set to new value
I (1586) esp_matter_bridge: Cluster plugin init for the new added endpoint
I (1596) chip[ZCL]: Boot reason: 0
I (1596) chip[ZCL]: Endpoint d On/off already set to new value
I (1606) esp_matter_bridge: Cluster plugin init for the new added endpoint
I (1606) chip[ZCL]: Boot reason: 0
I (1606) chip[ZCL]: Endpoint e On/off already set to new value
I (1616) esp_matter_bridge: Cluster plugin init for the new added endpoint
I (1626) chip[ZCL]: Boot reason: 0
I (1626) chip[ZCL]: Endpoint f On/off already set to new value
I (1696) gpio: GPIO[9]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 1| Pulldown: 0| Intr:0 
I (1696) gpio: GPIO[10]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0 
I (1706) esp_zboss: Button/LED init done, GPIO9 initial level: 1
I (1706) ESP_RADIO_SPINEL: spinel UART interface initialization completed
I (1706) ESP_RADIO_SPINEL: Spinel UART interface has been successfully enabled
                                                                                                                       I (1726) ESP_ZIGBEE_RADIO_SPINEL_UART: Spinel UART interface enable successfully
I (1716) main_task: Returned from app_main()
I(1736) OPENTHREAD:[I] P-SpinelDrive-: co-processor reset: RESET_POWER_ON
E(1736) OPENTHREAD:[C] P-SpinelDrive-: Software reset co-processor successfully
> I (1866) esp_zboss: status: -1
I (1866) esp_zboss: Zigbee stack initialized
I (1886) esp_zboss: status: 0
I (1886) esp_zboss: Start network formation
I (1896) esp_zboss: Formed network successfully
I (1896) esp_zboss: ieee extended address: 74:4d:bd:ff:fe:6e:6a:46, PAN ID: 0x5b1a)
I (1906) esp_zboss: Network Key (for Wireshark decryption):
I (1906) esp_zboss: 1e 14 e6 3f a2 52 ce 65 f6 29 fe 0a 10 0f 36 1e
I (1916) esp_zboss: 短按 BOOT 按键可开放入网 180 秒
I (19876) esp_zboss: status: 0
I (30946) chip[DL]: bleAdv Timeout : Start slow advertisement
I (30946) chip[DL]: Configuring CHIPoBLE advertising (interval 500 ms, connectable)
I (30946) chip[DL]: Device already advertising, stop active advertisement and restart
I (30966) NimBLE: GAP procedure initiated: stop advertising.

I (30966) NimBLE: GAP procedure initiated: advertise; 
I (30976) NimBLE: disc_mode=2
I (30976) NimBLE:  adv_channel_map=0 own_addr_type=1 adv_filter_policy=0 adv_itvl_min=800 adv_itvl_max=800
I (30986) NimBLE: 


mgt@mgt-virtual-machine:~/pro_esp/zigbee_matter_bridge$ 
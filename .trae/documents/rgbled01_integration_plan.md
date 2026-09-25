# 接入 RGBLED-01（UIID 0x0002，RGB 灯）实施方案

## Context（背景与目标）

桥接工程当前已完整接入 TH-01 温湿度传感器（UIID 0x0001），但要新增一款 **RGB 灯 RGBLED-01（UIID 0x0002）**。灯与传感器完全不同：灯是**执行器**，需要完整双向控制——Matter 端能下发开关/亮度/颜色，Zigbee 端状态变化也能同步回 Matter；且用**标准 ZCL 集群**（OnOff 0x0006 / LevelControl 0x0008 / ColorControl 0x0300）。

现状核实：
- app_main.cpp **已是完整固件**（git 工作区干净，HEAD=4d9841a），无需"恢复固件"。L183 的 CHIP console `#if 0` 是故意的（与 serial_cmd 共享 UART0 冲突），必须保留。
- 现有上行链路（Zigbee→Matter）：temp/humidity/on_off handler 经 `zcl_bridge_api_t` 依赖注入 → `bridge_manager` → `attribute::update`。仅 OnOff 已有状态上报，**Level/Color 未接入**。
- 现有下行链路（Matter→Zigbee）：`bridge_manager_attr_update` 是**空 stub**，需实现。
- 桥接设备固定按 `ESP_MATTER_TEMP_HUMIDITY_SENSOR_DEVICE_TYPE_ID` 创建，存在"先建传感器端点后读型号"的鸡生蛋问题，需解决才能建出 light 类型。

目标产物：RGBLED-01 被识别为 Matter ExtendedColorLight（0x010D，含 OnOff+Level+Color cluster），双向同步，且不影响 TH-01 现有行为。

---

## 改动清单（按实现顺序）

### 1. 设备类型识别：`main/matter/device_info.h` + `device_info.cpp`
- [device_info.h](file:///home/mgt/pro_esp/zigbee_matter_bridge/main/matter/device_info.h) L48 后新增：`#define UIID_RGB_LED 0x0002`
- [device_info.cpp](file:///home/mgt/pro_esp/zigbee_matter_bridge/main/matter/device_info.cpp) L346 `s_model_uiid_map[]` 新增一项：`{ "RGBLED-01", 0x0002 }`

### 2. 桥接接口扩容：`main/zigbee/zcl_bridge_api.h`
在 `zcl_bridge_api_t`（L38-53）`report_on_off` 之后新增三个函数指针（灯相关上报）：
- `void (*report_level)(uint16_t short_addr, uint8_t endpoint, uint8_t level);`
- `void (*report_color_xy)(uint16_t short_addr, uint8_t endpoint, uint16_t x, uint16_t y);`
- `void (*report_color_hsv)(uint16_t short_addr, uint8_t endpoint, uint8_t hue, uint8_t sat);`

### 3. 下行链路（Matter→Zigbee）：`main/matter/bridge_manager.cpp` `bridge_manager_attr_update`（L360-372）
按 `cluster_id + attribute_id` 分派，统一用**标准 ZCL command**（对执行器比写属性更健壮）。目标地址取自 `zigbee_device->dev_addr.zigbee_shortaddr / zigbee_endpointid`，`src_endpoint = HA_GW_ENDPOINT`，`address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT`。

| Matter cluster | Matter attribute | Zigbee 动作 | 函数（已验证存在于 managed_components esp-zigbee-lib） |
|---|---|---|---|
| OnOff::Id | OnOff | ON/OFF 命令 | `esp_zb_zcl_on_off_cmd_req` |
| LevelControl::Id | CurrentLevel | MOVE_TO_LEVEL（level=val, transition_time=0） | `esp_zb_zcl_level_move_to_level_cmd_req` |
| ColorControl::Id | CurrentX/CurrentY | MOVE_TO_COLOR（需要 X+Y 一对） | `esp_zb_zcl_color_move_to_color_cmd_req` |
| ColorControl::Id | CurrentHue/CurrentSaturation | MOVE_TO_HUE_AND_SATURATION（H+S 一对） | `esp_zb_zcl_color_move_to_hue_and_saturation_cmd_req` |

关键点：
- **颜色成对值凑齐**：Matter 控制器一次只写一个属性（如 CurrentX）。收到 CurrentX 更新时，用 `esp_matter::cluster::attribute::get_data(endpoint_id, ColorControl::Id, CurrentY::Id)` 读配对值，再组 MOVE_TO_COLOR（单一数据源，避免影子状态漂移）。Hue/Sat 同理。
- **ColorMode 语义反向**：Zigbee `0x00=XY,0x01=HS`；Matter `0x00=HS,0x01=XY`。颜色属性写入前/上报后都要 swap 0↔1。
- 保护：`val` 为 NULL 或 `zigbee_device->dev->endpoint` 为空直接 return；传感器端点无这些 cluster，天然不会误发。
- 参考既有 write 模式：[zdp_discovery.cpp](file:///home/mgt/pro_esp/zigbee_matter_bridge/main/zigbee/zdp_discovery.cpp) L466-479（write_attr）、L503-514（config_report），字段填充方式照抄。

### 4. 上行链路处理：新建 `main/zigbee/handlers/handler_level.cpp/.h` 和 `handler_color.cpp/.h`
参照现有大小 [handler_on_off.cpp](file:///home/mgt/pro_esp/zigbee_matter_bridge/main/zigbee/handlers/handler_on_off.cpp)（on_report_attr + 调 `api->xxx`）：
- **handler_level**：cluster 0x0008，report `ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID`（u8, 0-254）→ `api->report_level(short_addr, ep, level)`。
- **handler_color**：cluster 0x0300，report CURRENT_X/CURRENT_Y、CURRENT_HUE/CURRENT_SATURATION。因一次上报通常只带**一个**属性，handler 内维护按 short_addr 索引的**异构影子表**（x、y、hue、sat），凑齐一对才发 `api->report_color_xy(...)` / `api->report_color_hsv(...)`。COLOR_MODE 仅记录，用于区分 XY/HS。
- `zcl_device_handler_t` 类型复用现有 [zcl_handler_registry.h](file:///home/mgt/pro_esp/zigbee_matter_bridge/main/zigbee/zcl_handler_registry.h) L35 定义。

### 5. 注册 handler：`main/zigbee/zcl_handler_registry.cpp`
`zcl_handlers_init()`（L188-191）放开注释注册行：
- `zcl_handler_register(&level_control_handler); // 0x0008`
- `zcl_handler_register(&color_control_handler); // 0x0300`
并在顶部日志/展示列表补充。

### 6. 新建上游 report 透传：`main/matter/bridge_manager.cpp`
`zcl_bridge_api_impl`（L93-98）增加三个实现，内部用 `attribute::update` 写 Matter 属性：
- level → `LevelControl::Attributes::CurrentLevel::Id`（`ESP_MATTER_VAL_TYPE_NULLABLE_UINT8`，刻度一致）
- xy → `ColorControl::Attributes::CurrentX/CurrentY::Id`（`ESP_MATTER_VAL_TYPE_NULLABLE_UINT16`）
- hsv → `ColorControl::Attributes::CurrentHue/CurrentSaturation::Id`
OnOff 已有关不重复加。**注意**：写 Matter 的 ColorMode 前做 0↔1 swap。

### 7. 【核心】解决鸡生蛋 + 按 uiid 建 light 端点：`main/matter/bridge_manager.cpp`

**7a. `on_device_discovered`（L226-260）**：删除 L242-253 预创建 TEMP_HUMIDITY 桥接设备的整段（那时拿不到 uiid）。保留：已存在检查（L229-233）、`zboss_manager_register_bridged_device`（L256）、`zdp_discovery_read_basic_attributes`（L259）。

**7b. `on_device_identified`（L262-326）**：
- 新增辅助 `static uint32_t uiid_to_matter_device_type_id(uint16_t uiid)`：
  - `0x0001 → ESP_MATTER_TEMP_HUMIDITY_SENSOR_DEVICE_TYPE_ID`
  - `0x0002 → ESP_MATTER_EXTENDED_COLOR_LIGHT_DEVICE_TYPE_ID`（已验证 `=0x010D`）
  - 默认 → TEMP_HUMIDITY 兜底
- CASE 2（新设备，L292-323）：把"存在则复用 / 不存在防御性创建"两分支合并为——**若没有 bridge 则按 `uiid_to_matter_device_type_id(uiid)` 创建**，再统一 `device_info_register` + `set_matter_endpoint` + `zboss_manager_register_bridged_device`。
- CASE 1（重连，L268-290）逻辑不改（短地址更新、uiid 补抄已覆盖灯的重连）。
- **理由**：identified 是拿到 device_id+uiid 的第一现场，唯一能确定 device_type 的地方。创建时机从 discovered 挪到 identified，从根上消掉鸡生蛋，全程只 create 一次，无端点泄漏/重建。
- **注意**：此改动同时影响 TH-01（它的建点也移到 identified）。TH-01 在 Basic 读响应后必然走 identified，行为一致；若某设备 Basic 读失败不再有 eager 建点，属可接受的降级（identified 是唯一权威现场）。

### 8. 集群创建 case：`main/matter/bridge_manager.cpp` `bridge_manager_create_devices_cb`（L164-186）
新增 `case ESP_MATTER_EXTENDED_COLOR_LIGHT_DEVICE_TYPE_ID:`，创建三个 server 集群：
- `on_off::create(ep, &on_conf, CLUSTER_FLAG_SERVER)`
- `level_control::create(ep, &lvl_conf, CLUSTER_FLAG_SERVER)`（`current_level=0`，`on_level=254`）
- `color_control::create(ep, &color_conf, CLUSTER_FLAG_SERVER)`（`color_capabilities` 置 Hue/Sat+XY 位，`color_mode` 置匹配值）
此 case 同时服务"首次创建"和"重启 resume"（`app_bridge_initialize` 用持久化 light id 走 `set_device_type` 会再调本回调）。

### 9. 固件恢复
**不需要**。当前 app_main.cpp 已是完整固件。若后续你在别处出现极简测试版才需恢复，此处跳过。

---

## Configure Reporting：本期不做
灯是被动执行器，状态主要来自本网关命令，Matter 侧是被动方；只有"被第三方 Zigbee 控制器改动状态"才需要回灌上报，属二期增强。本期靠 handler 被动接收即可。二期可复用 [zdp_discovery.cpp](file:///home/mgt/pro_esp/zigbee_matter_bridge/main/zigbee/zdp_discovery.cpp) L489 `send_config_report` 基建。

---

## 验证方式（端到端）
1. `source /home/mgt/esp/esp-idf-v5.4.1/export.sh && source /home/mgt/esp/esp-matter/export.sh && IDF_COMPONENT_MANAGER_OFFLINE_SKIP_SOLVE=1 idf.py build` 编译通过。
2. 烧录后（`idf.py -p /dev/ttyACM0 flash monitor`，S3 手动下载模式），短按 GPIO9 开入网窗口，RGBLED-01 入网。
3. 串口应出现 `Model: RGBLED-01`、`uiid: 0x0002`、并创建 Matter light 端点日志。
4. Matter 控制器（如 Matter 配网 app/shell）里出现 ExtendedColorLight 设备，测试：
   - 开关（OnOff）→ 串口见下发 + 设备实际亮灭
   - 亮度（Level）→ MOVE_TO_LEVEL 生效
   - 颜色（Color）→ MOVE_TO_COLOR / MOVE_TO_HUE_AND_SATURATION 生效
5. 反向：遥控器/Zigbee 端改灯状态 → Matter 属性同步（report_level/color 链路日志）。
6. 回归：TH-01 温湿度传感器仍正常上报（证明建点时机后移无副作用）。
7. 重启网关验证：Matter 灯端点按持久化 light id resume，无重复建点、无"Endpoints are used up"。

---

## 风险与注意
1. **端点上限**：`MAX_BRIDGED_DEVICE_COUNT≈29`（CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=32 - aggregator）。灯+传感器未来逼近上限时需扩配置。
2. **ColorMode 语义反向**（0↔1），双向换算，勿直接赋值。
3. **颜色成对值时序**：单属性更新与命令需要的一对值间有窗口，务必用 `attribute::get_data` 读配对值凑齐，否则 MOVE_TO_COLOR 发出不完整色点。
4. Matter 端点在 Zigbee 读属性响应任务上下文创建，是本工程既有做法；建点失败需打 LOG/降级，勿静默。
5. **保留 L183 的 CHIP console `#if 0`**（UART0 冲突），勿误删/误恢复。
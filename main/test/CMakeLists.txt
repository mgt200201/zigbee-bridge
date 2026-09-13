# ================================================================
#  单元测试组件 — ZCL 路由核心 & 指标系统
#
#  大厂标准：所有核心逻辑必须有单元测试覆盖。
#  
#  运行方式（在项目根目录）：
#    idf.py build flash monitor
#  （单元测试随固件一起编译，启动后通过 console 触发）
#
#  覆盖范围：
#    - zcl_registry:  传感器表匹配、handler 注册/查找、分发逻辑
#    - zcl_metrics:    指标计数、环形缓冲区、错误分类
#    - zcl_bridge_api: 注册/获取接口
# ================================================================

idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS ".." "../zigbee" "../zigbee/private" "../matter"
    REQUIRES unity
)
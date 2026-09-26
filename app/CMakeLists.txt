# app 层（设计第 8/8.2/8.3/9.1/10/14 节）：Application State 单写者边界与
# 事件模型（M1-02）、Executor 生命周期 owner（M1-04，app/lifecycle）、宿主
# 组合根（M5-02，app/lifecycle/host_runtime——EUI-NEO 无关，GUI 钩子与
# console 驱动/关闭路径测试共享）。
# 头文件接口库；executor::comm 是跨上下文交付的指定设施（EXEC-03），
# 经 PUBLIC 依赖向 app 消费方（tests、Manager 与 UI 宿主）传播。
aki_add_skeleton_library(aki_app)
target_link_libraries(aki_app INTERFACE executor::executor)

# M5-02：宿主组合根实体库（设计 §9.1 首帧装配例外/启动↔关闭配对条款）。
# EUI-NEO 无关（RULE-10/RULE-01：console 测试 exe 链接本目标保持 console、
# 不加载显示栈，DEC-005「测试 exe 不链 eui」）；heyaki/persistence 符号经
# heyaki::client/aki_persistence 闭包提供。
add_library(aki_host STATIC lifecycle/host_runtime.cpp)
target_compile_features(aki_host PUBLIC cxx_std_20)
target_include_directories(aki_host PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aki_host PUBLIC
    aki_app
    aki_persistence
    aki_heyaki
    heyaki::client)
aki_apply_warnings(aki_host PRIVATE)

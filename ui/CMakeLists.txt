# EUI-NEO UI 层（设计第 9/9.1/14 节）：主窗口、页面、模型与主题。
#
# M5-03：ui/models 拆分为 EUI-NEO 无关的独立目标 aki_ui_models（纯 std/aki
# 类型——四域视图模型派生/快照消费面/UiActions 出站面），测试 exe 直链本
# 目标（DEC-005「测试 exe 不链 eui」的落地面）；aki_ui（eui::neo 唯一库面
# 消费点，RULE-01/RULE-10）只承载渲染/compose 面，exe 面经
# eui_neo_configure_app(aki) 链入（根 CMakeLists）。
add_library(aki_ui_models STATIC
    models/view_models.cpp
    models/ui_state_consumer.cpp
    models/ui_actions.cpp)
target_compile_features(aki_ui_models PUBLIC cxx_std_20)
target_include_directories(aki_ui_models PUBLIC ${PROJECT_SOURCE_DIR})
# 依赖方向（RULE-01）：ui/models -> Application（Manager 出站面头文件）->
# Domain；不依赖 eui。manager 头文件的 HeyakiAdapter SPI 经 aki_heyaki 传播。
target_link_libraries(aki_ui_models PUBLIC
    aki_app
    aki_persistence
    aki_heyaki)
aki_apply_warnings(aki_ui_models PRIVATE)

# M5-02：渲染/compose 面。测试 exe 不链 aki_ui（保持 console，DEC-005）——
# 主题数值回归对照由 EUI-NEO 无关的 ui/theme/aki_theme_values.hpp +
# tests/unit/test_ui_theme_values.cpp 承载。
add_library(aki_ui STATIC
    theme/aki_theme.cpp
    pages/main_window.cpp)
target_compile_features(aki_ui PUBLIC cxx_std_20)
target_include_directories(aki_ui PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aki_ui PUBLIC aki_ui_models eui::neo)
aki_apply_warnings(aki_ui PRIVATE)

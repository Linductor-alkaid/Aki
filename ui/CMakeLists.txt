# EUI-NEO UI 层（设计第 9/9.1/14 节）：主窗口、页面与主题。
# M5-02 实体化（DEC-005）：aki_ui 为 eui::neo 在库面的唯一消费点（RULE-01/
# RULE-10——eui 类型不越过 ui/ 公开面）；exe 面经 eui_neo_configure_app(aki)
# 链入（根 CMakeLists）。测试 exe 不链 aki_ui（保持 console，DEC-005）——
# 主题数值回归对照由 EUI-NEO 无关的 ui/theme/aki_theme_values.hpp +
# tests/unit/test_ui_theme_values.cpp 承载。
add_library(aki_ui STATIC
    theme/aki_theme.cpp
    pages/main_window.cpp)
target_compile_features(aki_ui PUBLIC cxx_std_20)
target_include_directories(aki_ui PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(aki_ui PUBLIC eui::neo)
aki_apply_warnings(aki_ui PRIVATE)

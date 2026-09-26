// 主窗口三栏壳与四页导航路由占位（设计 §9 三栏布局；M5-02 骨架——页面内容
// 占位，状态消费面与视图模型随 M5-03+ 落地）。
//
// 组合纪律（§9.1）：compose 只读派生——页面 UI 态（当前页/主题档）存于
// MainWindowModel（页面模型持有，重组时读入）；点击经回调改写模型后由
// 事件驱动的重组拾取（保留模式，M5-01 实测）。不等待、不轮询、不做 IO；
// 业务状态不在本层持有（M5-03 经视图模型派生接入）。
#pragma once

#include "ui/theme/aki_theme.hpp"

#include <eui/dsl.h>

#include <cstdint>
#include <string>

namespace aki::ui {

// 四页导航路由（§9 第一阶段导航集）。
enum class NavPage : std::uint8_t {    Conversations,
    Devices,
    Transfers,
    Settings,
};

// 页面模型（§9.1「页面持有 UI 态」；主线程 compose 上下文读写）。
struct MainWindowModel {
    NavPage page = NavPage::Conversations;
    ThemeMode theme = ThemeMode::Light;
    // HostRuntime 装配失败降级占位（§9.1 启动↔关闭配对：错误占位 UI + 关窗
    // 仍经 onShutdown 闭合）；空 = 装配成功。
    std::string startup_error;
};

// 三栏壳装配：导航栏(fixed 64) + 列表栏(fixed 264) + 内容栏(fill)，4px 可调
// 间隙的简化静态形态（§2.4 workspace layout；frame 不计圆角层级）。
void composeMainWindow(eui::Ui& ui, const eui::Screen& screen,
    MainWindowModel& model);

// 页面显示名（导航/列表栏标题共用）。
[[nodiscard]] const char* navPageTitle(NavPage page);

}  // namespace aki::ui

// 主窗口三栏壳与四页导航路由（设计 §9 三栏布局；M5-03 状态消费面最小接线
// ——四域视图模型计数展示 + 出站操作示范；页面具体展示归 M5-04~07）。
//
// 组合纪律（§9.1）：compose 只读派生——页面 UI 态（当前页/主题档/操作反馈）
// 与消费面状态（UiConsumerWatermark/UiStateSnapshot）、注入出站接口
// （UiActions）存于 MainWindowModel（页面模型持有，重组时读入）；点击经
// 回调改写模型后由事件驱动的重组拾取（保留模式，M5-01 实测）。compose 内
// 不等待、不轮询、不做 IO（消费与推进由宿主在 compose 前完成）；业务状态
// 不在本层持有——Store 经快照只读（RULE-02），操作全经 UiActions（DEC-008）。
#pragma once

#include "ui/models/ui_actions.hpp"
#include "ui/models/ui_state_consumer.hpp"
#include "ui/theme/aki_theme.hpp"

#include <eui/dsl.h>

#include <cstdint>
#include <memory>
#include <string>

namespace aki::ui {

// 四页导航路由（§9 第一阶段导航集）。
enum class NavPage : std::uint8_t {
    Conversations,
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

    // ---- M5-03 消费面与出站面（宿主装配，页面只读消费）----
    // 快照消费水位与最近派生视图（宿主 compose 前经 consume_ui_state 推进；
    // 页面模型持有 = 「页面持有 UI 态」纪律）。
    models::UiConsumerWatermark watermark;
    models::UiStateSnapshot state_view;
    // 注入出站接口（组合根绑定四 Manager 公开出站方法；页面不持有
    // Manager/transport 对象，RULE-01/RULE-02）。
    std::shared_ptr<models::UiActions> actions;
    // 页面持有的操作反馈文案（出站入队 admission 结果；RULE-09 拒绝可见）。
    std::string last_action_feedback;
    // 页面持有的信任确认弹窗态（M5-04）：待确认设备 id；空 = 弹窗关闭。
    // 弹窗展示该设备的 mono 指纹（=DeviceId 规范串），确认/拒绝经 UiActions。
    std::string pending_confirm_device;
};

// 三栏壳装配：导航栏(fixed 64) + 列表栏(fixed 264) + 内容栏(fill)，4px 可调
// 间隙的简化静态形态（§2.4 workspace layout；frame 不计圆角层级）。
void composeMainWindow(eui::Ui& ui, const eui::Screen& screen,
    MainWindowModel& model);

// 页面显示名（导航/列表栏标题共用）。
[[nodiscard]] const char* navPageTitle(NavPage page);

}  // namespace aki::ui

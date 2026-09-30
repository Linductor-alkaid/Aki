// Settings 页（M5-07，SCOPE-12）：主题三选（跟随系统/浅/深）+ 最小设置项
// 只读展示（数据目录/本地设备 id）。
//
// 组合纪律（§9.1）：主题选择为页面持有 UI 态（MainWindowModel.theme_setting，
// segmented 三选写入 + requestUpdate 唤醒拾取），生效经既有 akiTheme()/
// akiSemanticColors() 装配面（§9.1 覆写清单不变——仅档位选择，aki_ui_design
// §2 映射与回归对照维持）；FollowSystem 经平台查询（app/lifecycle/
// system_theme，点击回调上下文的有界注册表读取——非 compose 树构建内），
// Unknown 回落 Light 并页内披露。主题选择为会话级（不跨启动持久化——
// schema v1 无设置表，扩表属公开契约变更须先立决策）。最小设置项为快照/
// 装配面只读展示（数据目录 = HostRuntime::data_root()，本地设备 id = 快照
// UiStateSnapshot::local_device）——Store 经快照只读（RULE-02），页面不持
// transport（RULE-01），无 UiActions 出站需求。
#pragma once

#include "ui/models/ui_actions.hpp"
#include "ui/models/ui_state_consumer.hpp"
#include "ui/theme/aki_theme.hpp"

#include <eui/dsl.h>

#include <string>

namespace aki::ui {

struct MainWindowModel;  // settings_page.cpp 消费完整定义（main_window.hpp）。

// Settings 页（内容栏；main_window 三栏壳持有几何，数值入参传入）。
void composeSettingsPage(eui::Ui& ui,
    const components::theme::ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, float x, float y, float width,
    float height, MainWindowModel& model);

// Overlay belongs to the window root so the dialog centers in the window,
// independently of the Settings scroll position and content-column offset.
void composeSettingsPasswordDialog(eui::Ui& ui,
    const components::theme::ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, float screen_width,
    float screen_height, MainWindowModel& model);

}  // namespace aki::ui

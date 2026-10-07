// 高级折叠区开关（M8-01，DEC-028 决策 11 阶段 1）。EUI-NEO 无 disclosure/
// collapse 组件（组件面盘点见 M8 文档），以 ghost 按钮组合：chevron 方向
// 编码折叠态（§2.5 状态不仅靠颜色；码点按 §2.6 登记并经捆绑字体 cmap 实证）。
// 开合是纯页面 UI 态（§9.1「页面持有」）：onClick 只翻转标志，内容渲染/
// 隐藏由事件驱动的重组拾取（M5-01 契约）；compose 内无 IO、无派发。
#pragma once

#include "ui/theme/aki_theme.hpp"

#include "components/button.h"

#include <eui/dsl.h>

#include <string>
#include <utility>

namespace aki::ui {

// 折叠区开关：chevron-right（收起）/ chevron-down（展开），§2.6 登记码点
// f054/f077（2026-10-07 对 pinned 捆绑 FA7 Solid cmap 解析验证存在）。
inline void foldToggle(eui::Ui& ui, const std::string& id, bool& open,
    const components::theme::ThemeColorTokens& tokens, float x, float y,
    float width, float height, const std::string& label) {
    components::button(ui, id)
        .position(x, y)
        .size(width, height)
        .text(label)
        .icon(open ? eui::utf8(0xF077) : eui::utf8(0xF054))
        .textColor(tokens.text)
        .iconColor(tokens.text)
        .theme(tokens, false)
        .onClick([&open] { open = !open; })
        .build();
}

}  // namespace aki::ui

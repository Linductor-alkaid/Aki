// Transfers 页（M5-06，SCOPE-08 Transfers 页面）：传输任务集中列表与
// 暂停/恢复/取消操作面。
//
// 组合纪律（§9.1）：compose 只读派生——行渲染自最近消费快照的 TransferView
// 派生结果（consume_ui_state 水位推进，EXEC-03）；页面无独立 UI 态（无
// 弹窗/草稿/滚动代数需求——行高固定、无「回到底部」语义，滚动位置由运行
// 期状态保持），操作反馈经共享页面模型字段 MainWindowModel::
// last_action_feedback（RULE-09 拒绝可见；反馈行由 main_window 页尾统一
// 绘制）。业务状态不在本层持有——Store 经快照只读（RULE-02），操作全经
// UiActions 既有传输三接口（M5-03 建面；返回值即泵入队 admission）下达。
//
// 行形态：文件卡片本体 = ui/components/transfer_card 共享组件（aki_ui_design
// §4「会话内与 Transfers 页复用同一组件」，M5-05 ③ 形态迁移）；操作按钮按
// TransferView 状态门控（can_pause/can_resume/can_cancel，§7 固定边派生）：
// Paused 行的 Cancel 即 DEC-013⑥ 无会话行 cancel_transfer 直接终态入口的
// UI 触达（M4 唯一出口）。孤儿接收行 re-push 触发面为登记披露（M4-05/06
// 移交项——无对应 Manager 出站接口，页脚如实呈现，不冒充可用动作）。
// 页脚两行分行定位：登记披露行在跨页反馈行（main_window 页尾锚点）上方
// 一行（caption+tiny），两文本不叠印——反馈行恰需可见时披露行不遮蔽。
#pragma once

#include "ui/models/ui_actions.hpp"
#include "ui/models/ui_state_consumer.hpp"
#include "ui/theme/aki_theme.hpp"

#include <eui/dsl.h>

#include <string>

namespace aki::ui {

struct MainWindowModel;  // transfers_page.cpp 消费完整定义（main_window.hpp）。

// Transfers 页（内容栏；main_window 三栏壳持有几何，数值入参传入）。
void composeTransfersPage(eui::Ui& ui,
    const components::theme::ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, float x, float y, float width,
    float height, MainWindowModel& model);

}  // namespace aki::ui

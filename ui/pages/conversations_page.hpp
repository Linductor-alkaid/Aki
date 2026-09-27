// Conversations 页（M5-05，SCOPE-05/06/07 UI 面 + SCOPE-08 会话内文件卡片）：
// 列表栏会话列表 + 内容栏聊天窗口。
//
// 组合纪律（§9.1）：compose 只读派生——页面 UI 态（选中会话/输入草稿/滚动
// 代数/预览弹窗 open 态）与消息视图派生缓存存于 ConversationsPageModel（页面
// 模型持有，重组时读入；M5-01 waker 原型契约：dialog open 态页面持有 +
// requestUpdate 唤醒重组拾取）。compose 内不等待、不轮询、不做 IO；消息视图
// 派生（derive_message_views，纯函数）在快照水位/选中会话变化时于重组边界
// 重算，有界（消息预算 4096，RULE-09）。业务状态不在本层持有——Store 经
// 快照只读（RULE-02），操作全经 UiActions（DEC-008）；图片发送经文件对话框
// 只读选取（eui::platform::openFileDialog，M5-01 复核）→ hash-first 发起
// 链路（DEC-010/DEC-011）——对话框与文件 stat 在点击回调上下文（主线程事件
// 处理，非 compose 树构建内）执行。
//
// 消息历史滚动（M5-01 复核结论）：pinned virtuallist 为固定行高模型
// （virtuallist.h:35/:148），变高气泡列按「卡片自绘 + scrollview」组合承接，
// 不修改 pinned 依赖——气泡为 card（wrapContentHeight）+ text(wrap) 组合，
// 容器为 scrollview。滚动定位契约：pinned scrollview 的运行期滚动状态按
// 元素 id 持有（首次构建播种 offset，其后运行期所有）；页面以
// history_scroll_gen 代数进位切换 scrollview id 表达「回到底部」——选中
// 切换与新消息入流时 +1，用户滚动位置在两次重组之间由运行期保持。
#pragma once

#include "ui/models/ui_actions.hpp"
#include "ui/models/ui_state_consumer.hpp"
#include "ui/models/view_models.hpp"
#include "ui/theme/aki_theme.hpp"

#include <eui/dsl.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace aki::ui {

struct MainWindowModel;  // conversations_page.cpp 消费完整定义（main_window.hpp）。

// 出站图片本地源路径登记容量（RULE-09 预算；超限不再登记——预览退化为
// metadata 态，发送链路不受影响）。
inline constexpr std::size_t kOutboundSourceBudget = 64;

// Conversations 页页面模型（§9.1「页面持有 UI 态」；主线程 compose 上下文读写）。
struct ConversationsPageModel {
    // 选中会话（空 = 未选中：聊天窗口区显示会话空态）。
    aki::conversation::ConversationId selected;

    // 输入草稿（input 值镜像；onEnter/发送按钮消费后清空）。
    std::string draft;

    // 选中会话的消息视图流（快照 × 选中的派生缓存；水位/选中变化时重组边界
    // 重派生——纯函数派生，无 IO）。
    std::vector<models::MessageView> open_messages;
    aki::conversation::ConversationId derived_for;  // 缓存对应会话
    std::uint64_t derived_sequence = 0;             // 缓存对应快照水位
    std::size_t derived_count = 0;                  // 缓存对应消息条数（新消息检测）

    // 历史滚动代数（滚动契约见文件头）：选中切换/新消息入流时 +1。
    std::uint32_t history_scroll_gen = 0;

    // 新建会话弹窗（页面持有 open 态；Trusted 设备选择面）。false = 关闭。
    bool new_chat_open = false;

    // 图片预览弹窗（页面持有 open 态；空消息 id = 关闭）。
    aki::conversation::MessageId preview_message;

    // 出站图片本地源路径（发送时登记，预览弹窗消费；容量上限见
    // kOutboundSourceBudget）。
    std::vector<std::pair<aki::transfer::TransferId, std::filesystem::path>>
        outbound_sources;
};

// 会话列表（列表栏；main_window 三栏壳持有几何，数值入参传入）。
// window_width/height 为宿主窗口尺寸——弹窗（dialog）的背板/居中锚点
// （pinned dialog 默认 800x600 screen，M5-05 探针实测须显式传入）。
void composeConversationList(eui::Ui& ui,
    const components::theme::ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, float x, float y, float width,
    float height, float window_width, float window_height,
    MainWindowModel& model);

// 聊天窗口（内容栏；选中会话为空时渲染会话空态）。
void composeChatWindow(eui::Ui& ui,
    const components::theme::ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, float x, float y, float width,
    float height, float window_width, float window_height,
    MainWindowModel& model);

}  // namespace aki::ui

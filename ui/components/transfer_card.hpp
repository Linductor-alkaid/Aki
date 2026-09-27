// 传输卡片共享形态（aki_ui_design §4「文件卡片 | card + progress + button |
// 会话内与 Transfers 页复用同一组件」；M5-05 会话内气泡卡片实体化、M5-06
// 自 conversations_page 抽出为本共享单元——形状与语义色不变）。
//
// 落点：card 卡壳与排布归调用方（会话气泡内嵌 / Transfers 页行内），本单元
// 承载卡片本体的方向/文件名/大小·mime/进度条/状态文案与 §3 传输态语义色，
// 输出为单个 wrapContent stack（子元素绝对排版），可作 column 子项或行内
// 定位元素。语义色（§3）：Transferring brand / Paused warning / Failed
// destructive / Completed success / 其余（Queued·Negotiating·Cancelled）中性；
// untracked（会话内无传输行的单侧到达边角，DEC-010/DEC-013）以 warning
// 文案兜底、不渲染进度条——不猜测进度。
//
// EUI-NEO 类型仅经 aki_ui 消费（RULE-01/RULE-10）；页面只读渲染、无 IO 无
// 等待（§9.1 compose 三不纪律）。
#pragma once

#include "transfer/transfer/transfer_types.hpp"
#include "ui/theme/aki_theme.hpp"

#include <eui/dsl.h>

#include <cstdint>
#include <string>

namespace aki::ui::widgets {

// 卡片本体数据（两页共用的最小渲染面；调用方自视图模型填入）。
struct TransferCardData {
    bool outbound = false;  // 方向箭头（§2.6：发 f176 上 / 收 f175 下）。
    std::string file_name;
    std::string mime_type;
    std::uint64_t size_bytes = 0;
    // 传输行 join 结果（会话内消息卡片；Transfers 页行恒 tracked）。
    bool tracked = false;
    aki::transfer::TransferState state = aki::transfer::TransferState::Queued;
    // 进度 fraction ∈ [0,1]（total==0 → 0，不除零——与 TransferView 同口径）。
    double progress = 0.0;
};

// 字节量单位标注（B/KB/MB/GB，一位小数）——会话卡片与 Transfers 行共用。
[[nodiscard]] std::string format_bytes(std::uint64_t bytes);

// §3 传输态语义色（Transferring brand / Paused warning / Failed destructive
// / Completed success / 其余中性）。
[[nodiscard]] core::Color transfer_state_color(const AkiSemanticPalette& semantic,
    aki::transfer::TransferState state);

// 卡片本体（单个 wrapContent stack；宽度 = width，高度由子元素外延）。
void compose_transfer_card_body(eui::Ui& ui,
    const components::theme::ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, const std::string& id,
    const TransferCardData& data, float width);

}  // namespace aki::ui::widgets

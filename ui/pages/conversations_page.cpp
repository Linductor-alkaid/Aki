// Conversations 页实现（语义见 conversations_page.hpp；aki_ui_design §3 状态
// 视觉语义 / §4 组件映射：会话列表行=scrollview 行、消息气泡=card+text 组合
//（M5-01 复核：virtuallist 固定行高模型，变高气泡列按卡片自绘 + scrollview
// 承接）、图片消息=image+dialog 预览、文件卡片=card+progress+button（M5-06
// Transfers 页复用同一形态）、输入区=input+button）。
//
// 纪律：compose 只读派生（页面模型在重组边界经纯函数重派生消息视图，无
// 等待/轮询/IO）；文件对话框与文件 stat 只在点击回调上下文（主线程事件
// 处理）执行；出站操作全经 UiActions（RULE-01/RULE-02/DEC-008）。
#include "ui/pages/conversations_page.hpp"

#include "ui/components/transfer_card.hpp"
#include "ui/pages/main_window.hpp"

#include "components/button.h"
#include "components/card.h"
#include "components/dialog.h"
#include "components/image.h"
#include "components/input.h"
#include "components/progress.h"
#include "components/scrollview.h"
#include "components/text.h"

#include <eui/platform.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>

namespace aki::ui {
namespace {

using components::theme::ThemeColorTokens;

constexpr float kConvRowHeight = 64.0f;
constexpr float kBubbleMaxWidth = 340.0f;
constexpr float kBubbleWidthRatio = 0.72f;
constexpr float kLineHeightFactor = 1.35f;  // 文本行高经验档（引擎同量级）。

// ---- 快照只读 join（设备/会话预算 256，线性查找在预算内，RULE-09）----

const models::DeviceView* device_view_of(const MainWindowModel& model,
    const aki::device::DeviceId& id) {
    for (const models::DeviceView& device : model.state_view.devices) {
        if (device.id == id) {
            return &device;
        }
    }
    return nullptr;
}

const aki::conversation::Conversation* conversation_of(
    const MainWindowModel& model, const aki::conversation::ConversationId& id) {
    if (!model.state_view.has_snapshot) {
        return nullptr;
    }
    for (const aki::conversation::Conversation& conversation :
        model.state_view.state.conversations.conversations) {
        if (conversation.id == id) {
            return &conversation;
        }
    }
    return nullptr;
}

std::string remote_label(const MainWindowModel& model,
    const aki::device::DeviceId& remote) {
    const models::DeviceView* device = device_view_of(model, remote);
    return device != nullptr ? device->display_name : remote.value;
}

std::string path_label(aki::device::ConnectionPath path) {
    if (path == aki::device::ConnectionPath::Unknown) {
        return "--";
    }
    return std::string(aki::device::to_string(path));
}

std::string trust_label(aki::device::TrustState trust) {
    using aki::device::TrustState;
    switch (trust) {
    case TrustState::Unknown: return "Unknown";
    case TrustState::Pending: return "Pending";
    case TrustState::Trusted: return "Trusted";
    case TrustState::Rejected: return "Rejected";
    case TrustState::Revoked: return "Revoked";
    }
    return "Unknown";
}

core::Color trust_color(const AkiSemanticPalette& semantic,
    aki::device::TrustState trust) {
    using aki::device::TrustState;
    switch (trust) {
    case TrustState::Pending: return semantic.warning;      // §3 warning
    case TrustState::Trusted: return semantic.success;      // §3 success
    case TrustState::Rejected:
    case TrustState::Revoked: return semantic.destructive;  // §3 destructive
    case TrustState::Unknown: break;
    }
    return semantic.text_subtlest;                          // §3 subtlest
}

// §3 传输态语义色：Transferring brand / Paused warning / Failed destructive /
// Completed success / Queued·Negotiating·Cancelled 中性。

// §3 投递态图标（aki_ui_design §2.6 码点登记表）：Queued 时钟 / Sending
// paper-plane / Sent 单勾 / Delivered 双勾 / Failed 叹号。
std::string delivery_icon(aki::conversation::DeliveryState state) {
    using aki::conversation::DeliveryState;
    switch (state) {
    case DeliveryState::Queued: return eui::utf8(0xF017);
    case DeliveryState::Sending: return eui::utf8(0xF1D8);
    case DeliveryState::Sent: return eui::utf8(0xF00C);
    case DeliveryState::Delivered: return eui::utf8(0xF560);
    case DeliveryState::Failed: return eui::utf8(0xF06A);
    }
    return "";
}

core::Color delivery_color(const AkiSemanticPalette& semantic,
    aki::conversation::DeliveryState state) {
    using aki::conversation::DeliveryState;
    if (state == DeliveryState::Delivered) {
        return semantic.success;
    }
    if (state == DeliveryState::Failed) {
        return semantic.destructive;
    }
    return semantic.text_subtle;
}

std::string time_label(std::chrono::system_clock::time_point timestamp) {
    const std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
#if defined(_MSC_VER)
    if (localtime_s(&local, &time) != 0) {
        return "--:--";
    }
#else
    if (localtime_r(&time, &local) == nullptr) {
        return "--:--";
    }
#endif
    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", local.tm_hour,
        local.tm_min);
    return buffer;
}

// 出站图片本地源路径查找（预览弹窗；容量预算 kOutboundSourceBudget，线性）。
const std::filesystem::path* outbound_source_of(
    const ConversationsPageModel& chat,
    const aki::transfer::TransferId& transfer_id) {
    for (const auto& entry : chat.outbound_sources) {
        if (entry.first == transfer_id) {
            return &entry.second;
        }
    }
    return nullptr;
}

// ---- 消息视图派生缓存推进（重组边界；纯函数派生 + 页面模型写回，§9.1）----
// 水位/选中变化时重派生；选中切换或新消息入流时推进滚动代数（回到底部），
// 投递态等原位更新不推进（不打断用户阅读位置）。
void refresh_chat_stream(MainWindowModel& model) {
    ConversationsPageModel& chat = model.conversations;
    const aki::conversation::Conversation* conversation =
        conversation_of(model, chat.selected);
    if (!model.state_view.has_snapshot || conversation == nullptr) {
        chat.open_messages.clear();
        chat.derived_for = chat.selected;
        chat.derived_sequence = model.watermark.snapshot_sequence;
        chat.derived_count = 0;
        return;
    }
    if (chat.derived_for == chat.selected
        && chat.derived_sequence == model.watermark.snapshot_sequence) {
        return;  // 缓存新鲜。
    }
    chat.open_messages = models::derive_message_views(
        model.state_view.state.messages, *conversation,
        model.state_view.local_device, model.state_view.state.transfers);
    if (chat.derived_for != chat.selected) {
        ++chat.history_scroll_gen;  // 选中切换：回到底部。
    } else if (chat.open_messages.size() > chat.derived_count) {
        ++chat.history_scroll_gen;  // 新消息入流：跟随最新。
    }
    chat.derived_for = chat.selected;
    chat.derived_sequence = model.watermark.snapshot_sequence;
    chat.derived_count = chat.open_messages.size();
}

void set_feedback(MainWindowModel& model, std::string text) {
    model.last_action_feedback = std::move(text);
}

// ---- 出站操作（点击回调上下文；页面只经 UiActions，RULE-01/RULE-02）----

void send_draft(MainWindowModel& model, const aki::device::DeviceId& to) {
    if (!model.actions) {
        return;
    }
    if (model.conversations.draft.empty()) {
        set_feedback(model, "message is empty");
        return;
    }
    const auto message_id = model.actions->new_message_id();
    const bool admitted = model.actions->send_text(
        to, message_id, model.conversations.draft);
    set_feedback(model, admitted ? "text send admitted (" + message_id.value
                                       + ")"
                                 : "text send rejected (inbox admission)");
    if (admitted) {
        model.conversations.draft.clear();
    }
}

void pick_and_send_image(MainWindowModel& model,
    const aki::device::DeviceId& to) {
    if (!model.actions) {
        return;
    }
    // 文件对话框只读选取（M5-01 复核：openFileDialog 满足发送选取链路）。
    // 模态调用发生在主线程事件处理上下文（非 compose 树构建内）——compose
    // 三不纪律不破；模态期间帧更新暂停为本机 MVP 形态（如实登记）。
    eui::platform::FileDialogOptions options;
    options.prompt = "Select an image to send";
    options.filterName = "Images";
    options.allowedExtensions = {"png", "jpg", "jpeg", "gif", "webp", "bmp"};
    const eui::platform::FileDialogResult picked =
        eui::platform::openFileDialog(options);
    if (!picked.selected()) {
        set_feedback(model, "image pick cancelled");
        return;
    }
    const std::filesystem::path source{picked.paths.front()};
    std::error_code error;
    const auto bytes = std::filesystem::file_size(source, error);
    if (error) {
        set_feedback(model, "image stat failed: " + error.message());
        return;
    }
    const std::string extension = source.extension().string();
    std::string mime = "application/octet-stream";
    if (extension == ".png") {
        mime = "image/png";
    } else if (extension == ".jpg" || extension == ".jpeg") {
        mime = "image/jpeg";
    } else if (extension == ".gif") {
        mime = "image/gif";
    } else if (extension == ".webp") {
        mime = "image/webp";
    } else if (extension == ".bmp") {
        mime = "image/bmp";
    }
    aki::transfer::FileMetadata media;
    media.name = source.filename().string();
    media.size_bytes = bytes;
    media.mime_type = mime;
    const auto message_id = model.actions->new_message_id();
    const auto transfer_id = model.actions->new_transfer_id();
    // hash-first 发起链路（DEC-010/DEC-011）：先传输准入，消息等
    // stored_sha256 完成后经 TM 泵延续发出；false = 传输准入失败（消息行
    // 经编排补偿记 Failed）。
    const bool admitted = model.actions->send_image(
        to, message_id, media, transfer_id, source);
    set_feedback(model,
        admitted ? "image transfer admitted (" + transfer_id.value + ")"
                 : "image transfer admission failed");
    if (admitted
        && model.conversations.outbound_sources.size()
            < kOutboundSourceBudget) {
        model.conversations.outbound_sources.emplace_back(
            transfer_id, source);  // 预览登记（容量预算 RULE-09）。
    }
}

// ---- 消息气泡（§4：card + text 组合；己方/对方区分靠 surface 层级）----

void compose_bubble_meta(eui::Ui& ui, const ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, const std::string& id,
    const models::MessageView& message, float inner_width) {
    const auto& metrics = tokens.metrics;
    const float line_height = metrics.typography.micro * kLineHeightFactor;
    ui.stack(id)
        .size(inner_width, line_height)
        .content([&] {
            components::text(ui, id + ".time")
                .text(time_label(message.timestamp))
                .position(0.0f, 0.0f)
                .fontSize(metrics.typography.micro)
                .color(semantic.text_subtlest)
                .build();
            if (message.outbound) {
                // 投递徽标仅己方消息展示（§3：投递态是发送侧语义）。
                components::text(ui, id + ".delivery")
                    .icon(delivery_icon(message.delivery))
                    .position(inner_width - metrics.typography.micro, 0.0f)
                    .fontSize(metrics.typography.micro)
                    .color(delivery_color(semantic, message.delivery))
                    .build();
            }
        })
        .build();
}

// 文件卡片（Image/Video/File 气泡本体）：本体形态抽入共享组件
// ui/components/transfer_card（§4「会话内与 Transfers 页复用同一组件」，
// M5-06 落地抽取），本函数仅补会话侧增补面（图片 Preview 入口）。
void compose_file_card(eui::Ui& ui, const ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, const std::string& id,
    const models::MessageView& message, float inner_width,
    MainWindowModel& model) {
    const auto& metrics = tokens.metrics;
    ui.column(id)
        .width(inner_width)
        .wrapContent()
        .gap(metrics.spacing.tiny)
        .content([&] {
            widgets::TransferCardData data;
            data.outbound = message.outbound;
            data.file_name = message.media.name;
            data.mime_type = message.media.mime_type;
            data.size_bytes = message.media.size_bytes;
            data.tracked = message.transfer_tracked;
            data.state = message.transfer_state;
            data.progress = message.transfer_progress;
            widgets::compose_transfer_card_body(
                ui, tokens, semantic, id + ".body", data, inner_width);

            // 图片预览入口（§4 image+dialog；弹窗 open 态页面持有）。
            if (message.type == aki::conversation::MessageType::Image) {
                components::button(ui, id + ".preview")
                    .size(96.0f, metrics.control.menuItem)
                    .text("Preview")
                    .fontSize(metrics.typography.hint)
                    .theme(tokens, false)
                    .radius(metrics.radius.small)
                    .onClick([&model, message_id = message.id] {
                        model.conversations.preview_message = message_id;
                    })
                    .build();
            }
        })
        .build();
}

// 单条消息行（scrollview 内容列的子行；气泡宽度固定、高度 wrap——变高列由
// scrollview 内容列 wrapContent 承载，M5-01 复核结论的组合策略）。
void compose_message_row(eui::Ui& ui, const ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, const std::string& id,
    const models::MessageView& message, float content_width,
    MainWindowModel& model) {
    const auto& metrics = tokens.metrics;

    if (message.type == aki::conversation::MessageType::System) {
        // 系统消息：居中 caption，无气泡（行 stack 承载列内全宽——居中由
        // text horizontalAlign 在元素盒内完成）。
        ui.stack(id)
            .width(content_width)
            .wrapContent()
            .content([&] {
                components::text(ui, id + ".system")
                    .text(message.text)
                    .position(0.0f, 0.0f)
                    .fontSize(metrics.typography.caption)
                    .horizontalAlign(core::HorizontalAlign::Center)
                    .maxWidth(content_width)
                    .color(semantic.text_subtlest)
                    .build();
            })
            .build();
        return;
    }

    const float bubble_width =
        std::min(kBubbleMaxWidth, content_width * kBubbleWidthRatio);
    const float bubble_x =
        message.outbound ? content_width - bubble_width : 0.0f;
    const float inner_width =
        std::max(bubble_width - metrics.spacing.content * 2.0f, 40.0f);

    // 行包裹 stack（宽度 = 内容列宽）：scrollview 内容列为纵排布局、由其
    // 掌管子元素 x——气泡左右归属须在本行 stack 内绝对定位（M5-05 探针
    // 实测：直接把 card 挂进内容列会被列布局拉回左缘）。
    ui.stack(id)
        .width(content_width)
        .wrapContent()
        .content([&] {
            components::card(ui, id + ".bubble")
                .position(bubble_x, 0.0f)
                .width(bubble_width)
                .padding(metrics.spacing.content)
                // 己方/对方区分靠 surface 层级（§4）：己方 accent、对方
                // card+边框。
                .color(message.outbound ? semantic.accent : semantic.card)
                .radius(metrics.radius.card)
                .border(message.outbound ? 0.0f : 1.0f, tokens.border)
                .content([&] {
                    ui.column(id + ".stack")
                        .width(inner_width)
                        .wrapContent()
                        .gap(metrics.spacing.tiny)
                        .content([&] {
                            if (message.has_media) {
                                compose_file_card(ui, tokens, semantic,
                                    id + ".file", message, inner_width,
                                    model);
                            }
                            if (!message.text.empty()) {
                                components::text(ui, id + ".body")
                                    .text(message.text)
                                    .fontSize(metrics.typography.body)
                                    .wrap(true)
                                    .maxWidth(inner_width)
                                    .color(tokens.text)
                                    .build();
                            }
                            compose_bubble_meta(ui, tokens, semantic,
                                id + ".meta", message, inner_width);
                        })
                        .build();
                })
                .build();
        })
        .build();
}

// ---- 图片预览弹窗（§4 image+dialog；页面持有 open 态，M5-01 waker 原型
// 契约）----

void compose_preview_dialog(eui::Ui& ui, const ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, float window_width,
    float window_height, MainWindowModel& model) {
    ConversationsPageModel& chat = model.conversations;
    if (chat.preview_message.empty()) {
        return;
    }
    const models::MessageView* message = nullptr;
    for (const models::MessageView& view : chat.open_messages) {
        if (view.id == chat.preview_message) {
            message = &view;
            break;
        }
    }
    if (message == nullptr) {
        chat.preview_message = aki::conversation::MessageId{};  // 消息已不在缓存（快照更新）——关窗。
        return;
    }

    const auto& metrics = tokens.metrics;
    const float dialog_w = 480.0f;
    const float dialog_h = 480.0f;
    const float inner = dialog_w - metrics.spacing.section * 2.0f;

    components::dialog(ui, "aki.chat.preview")
        .open(true)
        .theme(tokens)
        .screen(window_width, window_height)  // 背板/居中锚点 = 宿主窗口。
        .size(dialog_w, dialog_h)
        // 背板点击的关闭请求回写页面持有 open 态（单向数据流：
        // 组件不私藏开关状态——M5-01 waker 契约的对称面；无 Escape
        // 路径——pinned dialog 仅背板 onClick 接 requestClose）。
        .onOpenChange([&model](bool open) {
            if (!open) {
                model.conversations.preview_message =
                    aki::conversation::MessageId{};
            }
        })
        .content([&] {
            components::text(ui, "aki.chat.preview.title")
                .text(message->media.name)
                .position(metrics.spacing.section, metrics.spacing.section)
                .fontSize(metrics.typography.subtitle)
                .fontWeight(600)
                .maxWidth(inner)
                .wrap(true)
                .color(tokens.text)
                .build();
            components::text(ui, "aki.chat.preview.meta")
                .text(widgets::format_bytes(message->media.size_bytes) + " · "
                    + (message->media.mime_type.empty()
                            ? "application/octet-stream"
                            : message->media.mime_type))
                .position(metrics.spacing.section,
                    metrics.spacing.section + metrics.typography.subtitle
                        + metrics.spacing.tiny)
                .fontSize(metrics.typography.caption)
                .color(semantic.text_subtle)
                .build();
            const std::filesystem::path* source =
                outbound_source_of(chat, message->transfer_id);
            const float body_y = metrics.spacing.section
                + metrics.typography.subtitle + metrics.typography.caption
                + metrics.spacing.content;
            if (source != nullptr) {
                components::image(ui, "aki.chat.preview.image",
                    components::ImageStyle(tokens))
                    .position(metrics.spacing.section, body_y)
                    .size(inner, inner)
                    .radius(metrics.radius.card)
                    .source(source->string())
                    .build();
            } else {
                // 接收侧/无本地路径：显式不可用态（接收文件落接收根，Store
                // 不持本地路径——如实呈现，不以占位图冒充）。
                components::text(ui, "aki.chat.preview.unavailable")
                    .text("local preview not available (received files land"
                          " in the receive root; metadata only)")
                    .position(metrics.spacing.section, body_y)
                    .fontSize(metrics.typography.caption)
                    .wrap(true)
                    .maxWidth(inner)
                    .color(semantic.text_subtlest)
                    .build();
                components::text(ui, "aki.chat.preview.sha")
                    .text(message->media.stored_sha256.empty()
                            ? std::string("sha-256: (pending sender archive)")
                            : "sha-256: " + message->media.stored_sha256)
                    .position(metrics.spacing.section,
                        body_y + metrics.typography.caption * 3.0f)
                    .fontSize(metrics.typography.hint)
                    .fontFamily("Mono")
                    .wrap(true)
                    .maxWidth(inner)
                    .color(semantic.text_subtle)
                    .build();
            }
            components::button(ui, "aki.chat.preview.close")
                .position(dialog_w - metrics.spacing.section - 120.0f,
                    dialog_h - metrics.control.field - metrics.spacing.section)
                .size(120.0f, metrics.control.field)
                .text("Close")
                .fontSize(metrics.typography.caption)
                .theme(tokens, true)
                .textColor(semantic.primary_foreground)
                .radius(metrics.radius.small)
                .onClick(
                    [&model] { model.conversations.preview_message = aki::conversation::MessageId{}; })
                .build();
        })
        .build();
}

}  // namespace

// ---- 会话列表（列表栏）----

void composeConversationList(eui::Ui& ui, const ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, float x, float y, float width,
    float height, float window_width, float window_height,
    MainWindowModel& model) {
    const auto& metrics = tokens.metrics;
    ConversationsPageModel& chat = model.conversations;

    // 新建会话入口（Trusted 设备选择面；§3 Trusted → 可进入会话）。
    components::button(ui, "aki.convs.new")
        .position(x + metrics.spacing.content, y)
        .size(width - metrics.spacing.content * 2.0f,
            metrics.control.menuItem)
        .text("New chat")
        .fontSize(metrics.typography.hint)
        .theme(tokens, false)
        .radius(metrics.radius.small)
        .onClick([&model] { model.conversations.new_chat_open = true; })
        .build();

    const float list_y = y + metrics.control.menuItem + metrics.spacing.compact;
    const float list_height =
        std::max(height - (list_y - y), metrics.control.menuItem);

    if (model.state_view.conversations.empty()) {
        components::text(ui, "aki.convs.empty")
            .text("No chats yet.\nPair a device first.")
            .position(x + metrics.spacing.content, list_y)
            .fontSize(metrics.typography.caption)
            .wrap(true)
            .maxWidth(width - metrics.spacing.content * 2.0f)
            .color(tokens.text)
            .build();
    } else {
        components::scrollView(ui, "aki.convs.list")
            .position(x + metrics.spacing.tiny, list_y)
            .size(width - metrics.spacing.tiny * 2.0f, list_height)
            .theme(tokens)
            .gap(metrics.spacing.tiny)
            .step(kConvRowHeight)
            .contentKey("aki.convs.rows:"
                + std::to_string(model.state_view.conversations.size()))
            .content([&](eui::Ui& list_ui, float row_width, float) {
                const auto& row_metrics = tokens.metrics;
                bool odd = false;
                for (const models::ConversationView& conversation :
                    model.state_view.conversations) {
                    const std::string row_id =
                        "aki.convs.row." + conversation.id.value;
                    const bool selected = chat.selected == conversation.id;
                    odd = !odd;
                    const models::DeviceView* remote =
                        device_view_of(model, conversation.remote_device);
                    // §3：Rejected/Revoked → 会话入口禁用（无点击面、
                    // destructive 信任徽标）。
                    const bool entry_disabled = remote != nullptr
                        && (remote->trust_state
                                == aki::device::TrustState::Rejected
                            || remote->trust_state
                                == aki::device::TrustState::Revoked);

                    list_ui.stack(row_id)
                        .size(row_width, kConvRowHeight)
                        .content([&] {
                            list_ui.rect(row_id + ".bg")
                                .size(row_width, kConvRowHeight)
                                .color(selected
                                        ? semantic.surface_overlay_strong
                                    : odd ? semantic.surface_overlay
                                          : semantic.card)
                                .radius(row_metrics.radius.small)
                                .build();
                            components::text(list_ui, row_id + ".name")
                                .text(remote_label(model,
                                    conversation.remote_device))
                                .position(row_metrics.spacing.content,
                                    row_metrics.spacing.compact)
                                .fontSize(row_metrics.typography.body)
                                .fontWeight(600)
                                .maxWidth(row_width
                                    - row_metrics.spacing.content * 2.0f)
                                .color(entry_disabled
                                        ? semantic.text_subtlest
                                        : tokens.text)
                                .build();
                            components::text(list_ui, row_id + ".time")
                                .text(conversation.last_message.has_value
                                        ? time_label(conversation.last_message
                                              .timestamp)
                                        : "")
                                .position(row_width
                                        - row_metrics.spacing.content - 40.0f,
                                    row_metrics.spacing.compact)
                                .fontSize(row_metrics.typography.micro)
                                .color(semantic.text_subtlest)
                                .build();
                            components::text(list_ui, row_id + ".preview")
                                .text(conversation.last_message.has_value
                                        ? conversation.last_message.preview
                                        : "no messages yet")
                                .position(row_metrics.spacing.content,
                                    row_metrics.spacing.compact
                                        + row_metrics.typography.body + 2.0f)
                                .fontSize(row_metrics.typography.caption)
                                .maxWidth(row_width
                                    - row_metrics.spacing.content * 2.0f
                                    - 48.0f)
                                .color(semantic.text_subtle)
                                .build();
                            // 己方最后消息的投递徽标（§3）。
                            if (conversation.last_message.has_value
                                && conversation.last_message.outbound) {
                                components::text(list_ui, row_id + ".delivery")
                                    .icon(delivery_icon(
                                        conversation.last_message.delivery))
                                    .position(row_width
                                            - row_metrics.spacing.content
                                            - 16.0f,
                                        row_metrics.spacing.compact
                                            + row_metrics.typography.body
                                            + 4.0f)
                                    .fontSize(row_metrics.typography.micro)
                                    .color(delivery_color(semantic,
                                        conversation.last_message.delivery))
                                    .build();
                            }
                            // Disconnected/Archived 徽标（§3）。
                            if (conversation.state
                                != aki::conversation::ConversationState::
                                    Active) {
                                components::text(list_ui, row_id + ".state")
                                    .text(std::string(
                                        aki::conversation::to_string(
                                            conversation.state)))
                                    .position(row_metrics.spacing.content,
                                        kConvRowHeight
                                            - row_metrics.spacing.compact
                                            - row_metrics.typography.micro)
                                    .fontSize(row_metrics.typography.micro)
                                    .color(conversation.state
                                            == aki::conversation::
                                                ConversationState::
                                                    Disconnected
                                        ? semantic.warning
                                        : semantic.text_subtlest)
                                    .build();
                            }
                            if (entry_disabled && remote != nullptr) {
                                components::text(list_ui, row_id + ".trust")
                                    .text(trust_label(remote->trust_state))
                                    .position(row_width
                                            - row_metrics.spacing.content
                                            - 70.0f,
                                        kConvRowHeight
                                            - row_metrics.spacing.compact
                                            - row_metrics.typography.micro)
                                    .fontSize(row_metrics.typography.micro)
                                    .color(trust_color(semantic,
                                        remote->trust_state))
                                    .build();
                            }
                            // 行点击面（透明按钮；Rejected/Revoked 无点击面
                            // ——§3 会话入口禁用）。
                            if (!entry_disabled) {
                                const core::Color transparent(0.0f, 0.0f,
                                    0.0f, 0.0f);
                                components::button(list_ui, row_id + ".hit")
                                    .position(0.0f, 0.0f)
                                    .size(row_width, kConvRowHeight)
                                    .text("")
                                    .theme(tokens, false)
                                    .radius(row_metrics.radius.small)
                                    .colors(transparent,
                                        semantic.surface_overlay_strong,
                                        transparent)
                                    .shadow(0.0f, 0.0f, 0.0f, transparent)
                                    .onClick([&model,
                                                 id = conversation.id] {
                                        if (model.conversations.selected
                                            == id) {
                                            return;
                                        }
                                        model.conversations.selected = id;
                                        model.conversations.draft.clear();
                                        model.conversations.preview_message =
                                            aki::conversation::MessageId{};
                                    })
                                    .build();
                            }
                        })
                        .build();
                }
            })
            .build();
    }

    // ---- 新建会话弹窗（Trusted 设备选择；页面持有 open 态）----
    if (!chat.new_chat_open) {
        return;
    }
    components::dialog(ui, "aki.convs.new_dialog")
        .open(true)
        .theme(tokens)
        .screen(window_width, window_height)  // 背板/居中锚点 = 宿主窗口。
        .size(420.0f, 340.0f)
        // 背板点击关闭请求回写页面模型（同预览弹窗；无 Escape 路径
        // ——pinned dialog 仅背板 onClick 接 requestClose）。
        .onOpenChange(
            [&model](bool open) { model.conversations.new_chat_open = open; })
        .content([&] {
            components::text(ui, "aki.convs.new_dialog.title")
                .text("New conversation")
                .position(metrics.spacing.section, metrics.spacing.section)
                .fontSize(metrics.typography.subtitle)
                .fontWeight(600)
                .color(tokens.text)
                .build();
            components::text(ui, "aki.convs.new_dialog.hint")
                .text("choose a trusted device")
                .position(metrics.spacing.section,
                    metrics.spacing.section + metrics.typography.subtitle
                        + metrics.spacing.tiny)
                .fontSize(metrics.typography.caption)
                .color(semantic.text_subtle)
                .build();
            float device_y = metrics.spacing.section
                + metrics.typography.subtitle + metrics.typography.caption
                + metrics.spacing.content;
            int trusted = 0;
            for (const models::DeviceView& device :
                model.state_view.devices) {
                if (device.trust_state != aki::device::TrustState::Trusted) {
                    continue;
                }
                ++trusted;
                components::button(ui,
                    "aki.convs.new_dialog.device." + device.id.value)
                    .position(metrics.spacing.section, device_y)
                    .size(420.0f - metrics.spacing.section * 2.0f,
                        metrics.control.menuItem)
                    .text(device.display_name)
                    .fontSize(metrics.typography.caption)
                    .theme(tokens, false)
                    .radius(metrics.radius.small)
                    .onClick([&model, remote = device.id] {
                        const bool admitted =
                            model.actions->ensure_conversation(
                                model.state_view.local_device, remote);
                        set_feedback(model,
                            admitted ? "conversation request admitted"
                                     : "conversation request rejected");
                        model.conversations.new_chat_open = false;
                    })
                    .build();
                device_y +=
                    metrics.control.menuItem + metrics.spacing.tiny;
                if (device_y > 340.0f - metrics.control.field
                    - metrics.spacing.section * 2.0f) {
                    break;  // 弹窗视口有界（设备预算 256，RULE-09；滚动选择
                            // 归 M5-06+ 通用列表形态）。
                }
            }
            if (trusted == 0) {
                components::text(ui, "aki.convs.new_dialog.none")
                    .text("no trusted devices yet — confirm pairing on the"
                          " Devices page first")
                    .position(metrics.spacing.section, device_y)
                    .fontSize(metrics.typography.caption)
                    .wrap(true)
                    .maxWidth(420.0f - metrics.spacing.section * 2.0f)
                    .color(semantic.text_subtlest)
                    .build();
            }
            components::button(ui, "aki.convs.new_dialog.close")
                .position(420.0f - metrics.spacing.section - 120.0f,
                    340.0f - metrics.control.field - metrics.spacing.section)
                .size(120.0f, metrics.control.field)
                .text("Cancel")
                .fontSize(metrics.typography.caption)
                .theme(tokens, false)
                .radius(metrics.radius.small)
                .onClick(
                    [&model] { model.conversations.new_chat_open = false; })
                .build();
        })
        .build();
}

// ---- 聊天窗口（内容栏）----

void composeChatWindow(eui::Ui& ui, const ThemeColorTokens& tokens,
    const AkiSemanticPalette& semantic, float x, float y, float width,
    float height, float window_width, float window_height,
    MainWindowModel& model) {
    const auto& metrics = tokens.metrics;
    ConversationsPageModel& chat = model.conversations;

    refresh_chat_stream(model);
    const aki::conversation::Conversation* conversation =
        conversation_of(model, chat.selected);

    if (conversation == nullptr) {
        components::text(ui, "aki.chat.empty")
            .text("Choose a chat from the list to begin.")
            .position(x + metrics.spacing.section, y + height * 0.5f)
            .fontSize(metrics.typography.body)
            .wrap(true)
            .maxWidth(width - metrics.spacing.section * 2.0f)
            .color(tokens.text)
            .build();
        return;
    }

    // ---- 会话头部（§4：text+徽标；元信息 subtle、路径 caption 中性）----
    components::text(ui, "aki.chat.title")
        .text(remote_label(model, conversation->remote_device))
        .position(x + metrics.spacing.section, y + metrics.spacing.section)
        .fontSize(metrics.typography.subtitle)
        .fontWeight(600)
        .color(tokens.text)
        .build();
    const models::DeviceView* remote =
        device_view_of(model, conversation->remote_device);
    const std::string chat_meta =
        path_label(remote != nullptr ? remote->connection_path
                                     : aki::device::ConnectionPath::Unknown)
        + " · "
        + std::string(
            aki::conversation::to_string(conversation->state));
    components::text(ui, "aki.chat.meta")
        .text(chat_meta)
        .position(x + metrics.spacing.section + 280.0f,
            y + metrics.spacing.section + 3.0f)
        .fontSize(metrics.typography.caption)
        .color(semantic.text_subtle)
        .build();
    if (remote != nullptr) {
        components::text(ui, "aki.chat.trust")
            .text(trust_label(remote->trust_state))
            .position(x + width - metrics.spacing.section - 80.0f,
                y + metrics.spacing.section + 3.0f)
            .fontSize(metrics.typography.caption)
            .color(trust_color(semantic, remote->trust_state))
            .build();
    }

    float history_y = y + metrics.spacing.section + metrics.typography.subtitle
        + metrics.spacing.compact;
    // §3：Conversation Disconnected → 会话头部 warning 横条「连接断开，等待
    // 恢复」（恢复不新建会话）。
    if (conversation->state
        == aki::conversation::ConversationState::Disconnected) {
        const float banner_height =
            metrics.typography.caption + metrics.spacing.compact * 2.0f;
        ui.rect("aki.chat.banner")
            .position(x + metrics.spacing.section, history_y)
            .size(width - metrics.spacing.section * 2.0f, banner_height)
            .color(semantic.warning)
            .radius(metrics.radius.small)
            .build();
        components::text(ui, "aki.chat.banner.text")
            .text(eui::utf8(0xF071) + std::string("  连接断开，等待恢复"))
            .position(x + metrics.spacing.section + metrics.spacing.content,
                history_y + metrics.spacing.compact - 1.0f)
            .fontSize(metrics.typography.caption)
            .color(core::Color(1.0f, 1.0f, 1.0f, 1.0f))
            .build();
        history_y += banner_height + metrics.spacing.compact;
    }

    // ---- 输入区（§4 input+button；主输入壳 rounded-2xl 批准例外）----
    const float input_y =
        y + height - metrics.control.field - metrics.spacing.section;
    const float button_w = metrics.control.field;
    const float buttons_row = button_w * 2.0f + metrics.spacing.compact * 2.0f;
    const float input_width =
        std::max(width - metrics.spacing.section * 2.0f - buttons_row
                - metrics.spacing.compact,
            120.0f);
    components::input(ui, "aki.chat.input")
        .position(x + metrics.spacing.section, input_y)
        .size(input_width, metrics.control.field)
        .theme(tokens)
        .value(chat.draft)
        .placeholder("Message...")
        .multiline(false)
        .onChange(
            [&model](const std::string& value) {
                model.conversations.draft = value;
            })
        .onEnter([&model, to = conversation->remote_device] {
            send_draft(model, to);
        })
        .build();
    if (model.actions) {
        components::button(ui, "aki.chat.pick")
            .position(x + metrics.spacing.section + input_width
                    + metrics.spacing.compact,
                input_y)
            .size(button_w, metrics.control.field)
            .text("")
            .icon(eui::utf8(0xF03E))  // §2.6 图片
            .fontSize(metrics.typography.body)
            .theme(tokens, false)
            .radius(metrics.radius.small)
            .onClick([&model, to = conversation->remote_device] {
                pick_and_send_image(model, to);
            })
            .build();
        components::button(ui, "aki.chat.send")
            .position(x + metrics.spacing.section + input_width
                    + metrics.spacing.compact + button_w
                    + metrics.spacing.compact,
                input_y)
            .size(button_w, metrics.control.field)
            .text("")
            .icon(eui::utf8(0xF1D8))  // §2.6 发送
            .fontSize(metrics.typography.body)
            .theme(tokens, true)
            .iconColor(semantic.primary_foreground)
            .radius(metrics.radius.small)
            .onClick([&model, to = conversation->remote_device] {
                send_draft(model, to);
            })
            .build();
    }

    // ---- 消息历史（M5-01 复核结论：卡片自绘 + scrollview 承接变高气泡列；
    // 滚动定位契约见 conversations_page.hpp 文件头）----
    const float history_height = std::max(
        input_y - metrics.spacing.content - history_y,
        metrics.control.menuItem);
    const std::string history_id =
        "aki.chat.history." + std::to_string(chat.history_scroll_gen);
    const std::string measure_key = history_id + ":"
        + std::to_string(chat.open_messages.size()) + ":"
        + (chat.open_messages.empty() ? std::string()
                                      : chat.open_messages.back().id.value);
    components::scrollView(ui, history_id)
        .position(x + metrics.spacing.section, history_y)
        .size(width - metrics.spacing.section * 2.0f, history_height)
        .theme(tokens)
        .gap(metrics.spacing.compact)
        .step(48.0f)
        .offset(1.0e9f)  // 运行期按 id 播种 offset（构建时 clamp 到 maxOffset
                         // = 底部）；代数进位即「回到底部」，用户滚动位置在
                         // 两次代数进位之间由运行期状态保持。
        .contentKey(measure_key)
        .content([&](eui::Ui& list_ui, float content_width, float) {
            int index = 0;
            for (const models::MessageView& message :
                model.conversations.open_messages) {
                compose_message_row(list_ui, tokens, semantic,
                    history_id + ".msg." + std::to_string(index++), message,
                    content_width, model);
            }
        })
        .build();

    compose_preview_dialog(ui, tokens, semantic, window_width,
        window_height, model);
}

}  // namespace aki::ui

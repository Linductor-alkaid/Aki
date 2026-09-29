#pragma once

#include <atomic>
#include <string>
#include <string_view>
#include <utility>

namespace aki::ui {

enum class Language { Chinese, English };

inline std::atomic<Language>& language_state() {
    static std::atomic<Language> value{Language::Chinese};
    return value;
}

inline void set_language(Language value) { language_state().store(value); }
inline Language language() { return language_state().load(); }

inline std::string tr(std::string_view english) {
    if (language() == Language::English) return std::string(english);
    static constexpr std::pair<std::string_view, std::string_view> entries[] = {
        {"Chat", "聊天"}, {"Conversations", "会话"}, {"Devices", "设备"},
        {"Chinese", "中文"}, {"English", "English"}, {"Language", "语言"},
        {"Files", "文件"}, {"Transfers", "传输"}, {"Settings", "设置"},
        {"Preferences", "偏好设置"}, {"New chat", "新建会话"},
        {"New conversation", "新建会话"}, {"Cancel", "取消"},
        {"Close", "关闭"}, {"Confirm", "确认"},
        {"Confirm pairing", "确认配对"}, {"Connect", "连接"},
        {"Reject", "拒绝"}, {"Revoke", "撤销信任"},
        {"Renew file access", "更新文件授权"},
        {"Save", "保存"}, {"Pause", "暂停"}, {"Resume", "继续"},
        {"Preview", "预览"}, {"Start scan", "开始扫描"},
        {"Stop scan", "停止扫描"}, {"Theme", "主题"},
        {"Light", "浅色"}, {"Dark", "深色"},
        {"Follow system", "跟随系统"}, {"Data directory", "数据目录"},
        {"Local device", "本设备"}, {"Device ID", "设备 ID"},
        {"Device name", "设备名称"}, {"My remark", "我的备注"},
        {"Optional local name", "可选的本地备注"},
        {"Set up this device", "设置本设备"},
        {"Choose a device name and a pairing password (8+ characters).",
         "设置设备名和配对密码（至少 8 个字符）。"},
        {"Device name must be 1-64 bytes without controls.",
         "设备名须为 1 至 64 字节，不能包含控制字符。"},
        {"Use at least 8 characters.", "请至少输入 8 个字符。"},
        {"Use at least 8 characters", "请至少输入 8 个字符"},
        {"Passwords do not match.", "两次密码不一致。"},
        {"Passwords do not match", "两次密码不一致"},
        {"Choose a password different from the old default.",
         "请设置不同于旧默认值的密码。"},
        {"Local pairing password", "本设备配对密码"},
        {"Confirm password", "确认密码"},
        {"Save password and start Aki", "保存并启动 Aki"},
        {"Find devices on your local network.", "查找局域网中的设备。"},
        {"Select a device to see its details and actions.",
         "选择设备后查看详情和操作。"},
        {"No chats yet.\nConnect a device first.",
         "暂无会话。\n请先连接设备。"},
        {"No connected devices yet. Connect one on the Devices page.",
         "暂无已连接设备。请先在设备页连接。"},
        {"choose a connected device", "选择已连接设备"},
        {"Choose a chat from the list to begin.", "从列表选择会话。"},
        {"No transfers yet. Send a file from a conversation to track it here.",
         "暂无传输。在会话中发送文件后可在这里查看。"},
        {"Change local pairing password", "修改本设备配对密码"},
        {"Other devices will need the new password when pairing.",
         "其他设备下次配对时需要输入新密码。"},
        {"New password (8+ characters)", "新密码（至少 8 个字符）"},
        {"Confirm new password", "确认新密码"},
        {"Save password", "保存密码"},
        {"Peer device password", "对方设备密码"},
        {"Password set on the peer", "对方设置的密码"},
        {"Enter the peer device password", "请输入对方设备密码"},
        {"Verify the fingerprint on the peer device.",
         "请核对与对方设备显示的指纹是否一致。"},
        {"Theme choice lasts this session.", "主题设置仅在本次运行期间有效。"},
        {"Follow system uses Windows settings.\nOther systems use Light.\nTheme choice lasts this session.",
         "跟随系统读取 Windows 设置。\n其他系统使用浅色主题。\n主题设置仅在本次运行期间有效。"},
        {"Interrupted incoming files may need resending.\nPartial files may remain in local storage.",
         "中断的接收文件可能需要重发。\n本地可能保留部分文件。"},
        {"startup failed", "启动失败"},
        {"Unknown", "未知"}, {"Pending", "待确认"},
        {"Trusted", "已信任"}, {"Rejected", "已拒绝"},
        {"Revoked", "已撤销"}, {"Online", "在线"},
        {"Offline", "离线"}, {"Desktop", "台式机"},
        {"Tablet", "平板"}, {"Phone", "手机"},
        {"Server", "服务器"}, {"Robot", "机器人"},
        {"Device", "设备"}, {"Send", "发送"},
        {"Image", "图片"}, {"File", "文件"},
        {"Remark saved", "备注已保存"},
        {"Remark was not saved", "备注保存失败"},
        {"Connection request submitted for ", "已提交连接请求："},
        {"Connection request rejected", "连接请求未被接收"},
        {"Reject submitted for ", "已提交拒绝请求："},
        {"Reject request rejected", "拒绝请求未被接收"},
        {"Revoke submitted for ", "已提交撤销请求："},
        {"Revoke request rejected", "撤销请求未被接收"},
        {"Discovery request submitted", "已提交扫描请求"},
        {"Discovery request rejected", "扫描请求未被接收"},
        {"Stop request submitted", "已提交停止扫描请求"},
        {"Stop request rejected", "停止扫描请求未被接收"},
        {"Pairing submitted for ", "已提交配对请求："},
        {"Pairing request rejected", "配对请求未被接收"},
        {"Language save failed", "语言设置保存失败"},
        {"Local pairing password updated", "本设备配对密码已更新"},
        {"Select an image to send", "选择要发送的图片"},
        {"Select a file to send", "选择要发送的文件"},
        {"Images", "图片"},
        {"message is empty", "消息内容为空"},
        {"text send admitted (", "已提交消息（"},
        {"text send rejected (inbox admission)", "消息发送请求未被接收"},
        {"file picker failed: ", "文件选择失败："},
        {"file pick cancelled", "已取消文件选择"},
        {"file stat failed: ", "读取文件信息失败："},
        {"transfer admitted (", "已提交文件传输（"},
        {"transfer admission failed", "文件传输请求未被接收"},
        {"conversation request admitted", "已提交新会话请求"},
        {"conversation request rejected", "新会话请求未被接收"},
        {"pause admitted (", "已提交暂停请求（"},
        {"pause rejected (inbox admission)", "暂停请求未被接收"},
        {"resume admitted (", "已提交继续请求（"},
        {"resume rejected (inbox admission)", "继续请求未被接收"},
        {"cancel admitted (", "已提交取消请求（"},
        {"cancel rejected (inbox admission)", "取消请求未被接收"},
        {"local preview not available (received files land in the receive root; metadata only)",
         "无法在此预览；收到的文件已保存到接收目录。"},
        {"Message...", "输入消息…"}, {"no messages yet", "暂无消息"},
        {"Queued", "排队中"}, {"Negotiating", "协商中"},
        {"Transferring", "传输中"}, {"Paused", "已暂停"},
        {"Completed", "已完成"}, {"Failed", "失败"},
        {"Cancelled", "已取消"}, {"Active", "活跃"},
        {"Disconnected", "已断开"}, {"Archived", "已归档"},
        {"no transfer row (single-side arrival)", "仅单侧收到文件，暂无传输记录"},
        {"Fingerprint unavailable", "设备指纹不可用"},
        {"Manage trusted peers.", "管理已发现和已连接设备。"},
        {"File activity.", "查看文件传输。"},
        {"Theme and local data.", "主题与本地数据。"},
        {"Pairing failed - retry", "配对失败，请重试"},
        {"(data directory unavailable)", "（数据目录不可用）"},
        {"(identity unavailable)", "（设备身份不可用）"},
    };
    for (const auto& [key, value] : entries)
        if (key == english) return std::string(value);
    return std::string(english);
}

inline std::string tr_preview(std::string_view preview) {
    if (language() == Language::English) return std::string(preview);
    for (const auto& [prefix, localized] : {
            std::pair<std::string_view, std::string_view>{"[image] ", "[图片] "},
            {"[video] ", "[视频] "}, {"[file] ", "[文件] "}}) {
        if (preview.starts_with(prefix))
            return std::string(localized) + std::string(preview.substr(prefix.size()));
    }
    return tr(preview);
}

}  // namespace aki::ui

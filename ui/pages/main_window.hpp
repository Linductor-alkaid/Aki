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
#include "ui/i18n.hpp"
#include "ui/models/ui_state_consumer.hpp"
#include "ui/pages/conversations_page.hpp"
#include "ui/pages/settings_page.hpp"
#include "ui/pages/transfers_page.hpp"
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
    // 生效档位（akiTheme()/akiSemanticColors() 参数；由 theme_setting 解析，
    // M5-07）。
    ThemeMode theme = ThemeMode::Light;
    Language language = Language::Chinese;
    // 主题三选 UI 态（M5-07 Settings 页 segmented；§9.1 页面持有 UI 态）。
    ThemeSetting theme_setting = ThemeSetting::Light;
    // 数据目录（HostRuntime::data_root() 装配面；Settings 页只读展示，
    // std::string 公开面 RULE-10）。
    std::string data_directory;
    bool profile_probe_failed = false;
    bool retry_profile_probe = false;
    // HostRuntime 装配失败降级占位（§9.1 启动↔关闭配对：错误占位 UI + 关窗
    // 仍经 onShutdown 闭合）；空 = 装配成功。
    std::string startup_error;
    bool needs_password_setup = false;
    bool language_selection_pending = false;
    std::string local_device_name_draft;
    std::string initial_device_name;
    // Settings 页本机名改名草稿（dirty=true 后显示草稿，可清空重输；
    // 保存成功后清空并回显状态中的新名）。
    std::string settings_device_name_draft;
    bool settings_name_dirty = false;
    std::string local_password_draft;
    std::string local_password_confirm;
    std::string initial_password;
    std::string password_feedback;
    bool password_change_open = false;
    std::string peer_password_draft;
    std::string peer_password_feedback;
    // M7/DEC-028：Settings 页中继区与 TURN 高级区草稿（启动预填 TURN/
    // 租户默认；保存按状态重置；token/凭据为会话内凭据，用后擦除——
    // secureInput + clear_secret，DEC-018 同款纪律）。
    std::string settings_relay_url_draft;
    std::string settings_relay_tenant_draft = "aki";
    std::string settings_relay_token_draft;
    std::string settings_relay_ca_draft;
    std::string settings_turn_host_draft;
    std::string settings_turn_port_draft;
    std::string settings_turn_username_draft;
    std::string settings_turn_credential_draft;

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
    std::string selected_device_id;
    std::string remark_draft;
    // Conversations 页页面模型（M5-05：选中会话/输入草稿/消息视图缓存/
    // 弹窗 open 态；语义见 conversations_page.hpp）。
    ConversationsPageModel conversations;
};

// 三栏壳装配：导航栏固定 64，列表栏随窗口宽度在 168~264 间调整，
// 内容栏填满余量；栏间 4px（§2.4 workspace layout）。
void composeMainWindow(eui::Ui& ui, const eui::Screen& screen,
    MainWindowModel& model);

// 页面显示名（导航/列表栏标题共用）。
[[nodiscard]] const char* navPageTitle(NavPage page);

}  // namespace aki::ui

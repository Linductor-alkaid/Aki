// Aki GUI 宿主入口（M5-02：自 console 宿主迁移为 EUI-NEO 框架 main 钩子，
// DEC-005「宿主入口重构」；原 console 冒烟/演示断言由 aki_host_smoke 承载）。
//
// 形态（设计 §9.1 装配契约，M5-02 增补条款）：
//   - 框架 main（glfw_app_main.cpp，eui_neo_configure_app 注入，/SUBSYSTEM:
//     WINDOWS，无 argv）→ dslAppConfig() 静态装配 → 主循环首帧 compose()。
//   - 首次 compose（主线程）：HostRuntime::ensure_assembled()——§8.3 七步
//     组合根 + 数据根解析（缺省 resolve_data_root()，GUI 无 argv 注入）。
//     「compose 三不纪律」的唯一显式例外（有界启动工作单元，设计 §9.1）。
//   - DslAppConfig::onShutdown（主线程，GPU 设备销毁前）：薄委托同一
//     HostRuntime 单例按 §8.3 钩子原序 + EXEC-01 步骤 2~5 受控关闭——
//     「装配成功 ⇒ 关闭必经 onShutdown」由框架控制流保证（主循环一切退出
//     路径汇入 app::shutdown()）。
//   - 窗口/GPU 销毁次序：onShutdown 返回后框架继续 app::shutdown() 收尾
//     （core::async → handler → dslRuntime → network）再 renderBackend.reset()
//     销毁 GPU 设备（glfw_app_main.cpp:594-604）——worker 回收（钩子内，
//     EXEC-01 步骤 2/3）先于 GPU 销毁，本日志序为证据（RULE-11 本机归档）。
//
// 证据（RULE-11：渲染不进 CI）：aki-run.log 随关键事件落盘 + flush（GUI 子系统
// 无控制台）；启动装配耗时、主题覆写对拍、onShutdown 关闭序与关闭报告均在
// 日志内，复现命令见 M5-02 验证记录。
#include "app/lifecycle/host_runtime.hpp"
#include "app/lifecycle/system_theme.hpp"
#include "heyaki/adapter/local_identity.hpp"
#include "persistence/storage/data_root.hpp"
#include "ui/components/secure_input.hpp"
#include "ui/models/ui_actions.hpp"
#include "ui/models/ui_state_consumer.hpp"
#include "ui/pages/main_window.hpp"
#include "ui/platform/application_identity.hpp"
#include "ui/platform/ime_guard.hpp"
#include "ui/platform/local_files.hpp"
#include "ui/theme/aki_theme.hpp"

#include <eui/dsl_app.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <exception>
#include <memory>
#include <string>

namespace {

// Per-user diagnostics must remain writable in installed Program Files / /opt builds.
std::string g_log_path;
void initialize_log_path() {
    try {
        const auto logs = std::filesystem::path{aki::persistence::resolve_data_root()} / "logs";
        std::filesystem::create_directories(logs);
        g_log_path = (logs / "aki-run.log").string();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Aki: cannot initialize diagnostics: %s\n", error.what());
    }
}

// Public GLFW hints must precede the framework's main/glfwInit. This does
// not initialize a window, task, or second platform lifecycle.
[[maybe_unused]] const bool g_window_backend_policy = [] {
    aki::ui::platform::configure_window_backend_before_init();
    return true;
}();

std::string steady_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(ms);
}

void log_line(const std::string& line) {
#if defined(_MSC_VER)
    FILE* file = nullptr;
    if (fopen_s(&file, g_log_path.c_str(), "a") != 0 || file == nullptr) {
        std::fprintf(stderr, "Aki: %s\n", line.c_str());
        return;
    }
#else
    FILE* file = std::fopen(g_log_path.c_str(), "a");
    if (file == nullptr) {
        std::fprintf(stderr, "Aki: %s\n", line.c_str());
        return;
    }
#endif
    std::fprintf(file, "[%sms] %s\n", steady_ms().c_str(), line.c_str());
    std::fflush(file);
    std::fclose(file);
}

// 页面模型（§9.1「页面持有 UI 态」：当前页/主题档/装配降级文案；主线程
// compose 上下文读写，函数级 static 与宿主单例同为进程生命周期）。
// M5-03：消费水位 + 最近派生视图（UiStateSnapshot）与注入出站接口
// （UiActions）同属页面模型持有面。
aki::ui::MainWindowModel& main_window_model() {
    static aki::ui::MainWindowModel model;
    return model;
}

// 出站接口（M5-03，设计 §9.1「操作一律经 Application 出站面」）：组合根
// 绑定一次（Manager 访问器直呼公开出站方法），页面经模型读取——页面不持
// Manager/transport 对象（RULE-01/RULE-02）。
std::shared_ptr<aki::ui::models::UiActions>& ui_actions() {
    static std::shared_ptr<aki::ui::models::UiActions> actions;
    return actions;
}

// 主题覆写回归对照（M5-01 探针同款对拍落盘：上游默认 → akiTheme 覆写值；
// §9.1「回归对照留档」+ aki_theme_values.hpp 数值表共同构成回归锚点）。
void log_theme_override() {
    const auto defaults = components::theme::light();
    const auto aki = aki::ui::akiTheme(aki::ui::ThemeMode::Light);
    char buffer[512] = {};
    std::snprintf(buffer, sizeof(buffer),
        "theme: typography.title %.0f->%.0f subtitle %.0f->%.0f body "
        "%.0f->%.0f caption %.0f->%.0f hint %.0f->%.0f micro %.0f->%.0f | "
        "radius.small %.0f->%.0f | control.field %.0f->%.0f menuItem "
        "%.0f->%.0f",
        defaults.metrics.typography.title, aki.metrics.typography.title,
        defaults.metrics.typography.subtitle, aki.metrics.typography.subtitle,
        defaults.metrics.typography.body, aki.metrics.typography.body,
        defaults.metrics.typography.caption, aki.metrics.typography.caption,
        defaults.metrics.typography.hint, aki.metrics.typography.hint,
        defaults.metrics.typography.micro, aki.metrics.typography.micro,
        defaults.metrics.radius.small, aki.metrics.radius.small,
        defaults.metrics.control.field, aki.metrics.control.field,
        defaults.metrics.control.menuItem, aki.metrics.control.menuItem);
    log_line(buffer);
    // §2.2 间距六档与上游默认一致（零覆写声明，对拍落盘）。
    std::snprintf(buffer, sizeof(buffer),
        "theme: spacing tiny %.0f compact %.0f content %.0f section %.0f "
        "large %.0f panel %.0f (aki==default)",
        aki.metrics.spacing.tiny, aki.metrics.spacing.compact,
        aki.metrics.spacing.content, aki.metrics.spacing.section,
        aki.metrics.spacing.large, aki.metrics.spacing.panel);
    log_line(buffer);
}

void log_assembly(const aki::app::HostAssemblyReport& report,
    std::chrono::milliseconds elapsed) {
    char buffer[512] = {};
    std::snprintf(buffer, sizeof(buffer),
        "boot: assembly %s in %lldms (devices=%zu conversations=%zu "
        "messages=%zu transfers=%zu migrations=%zu tmp_removed=%zu "
        "identity=%s lan=%d)",
        report.ok ? "ok" : "FAILED",
        static_cast<long long>(elapsed.count()),
        report.recovered_devices, report.recovered_conversations,
        report.recovered_messages, report.recovered_transfers,
        report.migrations_applied, report.tmp_orphans_removed,
        report.identity_created ? "created" : "loaded",
        report.lan_interfaces ? 1 : 0);
    log_line(buffer);
    if (!report.ok) {
        log_line("boot: assembly failure: " + report.failure_reason);
    }
}

void log_shutdown(const aki::app::HostShutdownReport& report,
    std::chrono::milliseconds elapsed) {
    char buffer[512] = {};
    std::snprintf(buffer, sizeof(buffer),
        "shutdown: hook_sequence_completed=%d fully_stopped=%d "
        "workers=%zu/%zu state_closed=%d db_drained=%d write "
        "admitted=%llu settled_failures=%llu language_write_failed=%d (elapsed %lldms)",
        report.hook_sequence_completed ? 1 : 0,
        report.executor_report.fully_stopped() ? 1 : 0,
        report.executor_report.blocking_workers_requested,
        report.executor_report.blocking_workers_stopped,
        report.state_owner_closed ? 1 : 0,
        report.db_drained_within_budget ? 1 : 0,
        static_cast<unsigned long long>(report.write_admitted),
        static_cast<unsigned long long>(report.write_settle_failures),
        report.language_write_failed ? 1 : 0,
        static_cast<long long>(elapsed.count()));
    log_line(buffer);
    for (const std::string& step : report.hook_sequence) {
        log_line("shutdown: " + step);
    }
    // 次序证据：worker 回收（EXEC-01 步骤 2/3，钩子内完成）先于 GPU 设备
    // 销毁（onShutdown 返回后框架 renderBackend.reset()，
    // glfw_app_main.cpp:603）。
    log_line("shutdown: onShutdown returns; framework proceeds to"
             " dslRuntime/network teardown then GPU device destruction");
}

}  // namespace

const app::DslAppConfig& app::dslAppConfig() {
    // 框架在 glfwInit 成功后、创建主窗口前首次查询配置；DEC-024 在这个
    // 有界启动点补齐 Linux 窗口身份（EUI-20260929-002）。后续每帧查询
    // 不再修改创建参数，也不装配 Host 或改变 GLFW 生命周期。
    static const bool first = [] {
        initialize_log_path();
        aki::ui::platform::configure_application_identity();
#if defined(__linux__)
        log_line(std::string("boot: GLFW platform=")
            + (glfwGetPlatform() == GLFW_PLATFORM_X11 ? "X11" : "Wayland/other"));
#endif
        log_line("boot: dslAppConfig constructed (EUI-NEO framework main)");
        return true;
    }();
    static const app::DslAppConfig config = app::DslAppConfig{}
        .title("Aki")
        .windowSize(1080, 720)
        .minWindowSize(880, 560)
        // 窗口/任务栏图标：Heyaki 图标（2026-09-27 用户确认；资产溯源见
        // assets/icons/README.md，POST_BUILD 随 exe 部署）。不设置时框架
        // 回落 EUI-NEO assets/icon.png。exe 内嵌图标同源（aki-icon.ico），
        // 经根 CMakeLists 覆写 EUI_NEO_APP_ICON_RESOURCE 注入。
        .iconPath("assets/icons/aki-icon.png")
        // 托盘 M5-02 不启用（DslAppConfig tray 默认 false；启用时须复验
        // tray exit 同汇入 app::shutdown 的关闭配对，设计 §9.1）。
        .onShutdown([] {
            // 主线程、GPU 设备销毁前（dsl_app_impl.h app::shutdown() 序：
            // core::async → handler → dslRuntime → network；对禁用 app::
            // async 的 Aki 无影响，DEC-005 并发边界）。
            log_line("shutdown: onShutdown entry (main thread, before"
                     " GPU device teardown)");
            auto& host = aki::app::HostRuntime::instance();
            const auto started = std::chrono::steady_clock::now();
            const auto& report = host.shutdown_with_report();
            log_shutdown(report,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started));
        });
    (void)first;
    return config;
}

void app::compose(eui::Ui& ui, const eui::Screen& screen) {
    static int frames = 0;
    static bool setup_checked = false;
    static bool assembly_started = false;
    ++frames;
    auto& model = main_window_model();
    auto& host = aki::app::HostRuntime::instance();
    aki::ui::platform::install_ime_backspace_guard();

    if (model.retry_profile_probe) {
        model.retry_profile_probe = false;
        model.profile_probe_failed = false;
        model.startup_error.clear();
        setup_checked = false;
    }
    if (!setup_checked) {
        setup_checked = true;
        try {
            model.data_directory = aki::persistence::resolve_data_root();
            {
                std::ifstream preference(model.data_directory + "/ui-language.txt");
                std::string code;
                if (preference >> code && code == "en")
                    model.language = aki::ui::Language::English;
                aki::ui::set_language(model.language);
            }
            model.needs_password_setup =
                aki::heyaki::LocalProfile::requires_password_setup(
                    model.data_directory);
            model.language_selection_pending = model.needs_password_setup;
            log_line("startup: profile data_root=" + model.data_directory
                + "; password_setup_required=" + (model.needs_password_setup ? "yes" : "no"));
        } catch (const std::exception& error) {
            model.profile_probe_failed = true;
            model.startup_error = std::string("Cannot inspect local device profile: ") + error.what();
            log_line("startup: " + model.startup_error);
        } catch (...) {
            model.profile_probe_failed = true;
            model.startup_error = "Cannot inspect local device profile (unknown error).";
            log_line("startup: " + model.startup_error);
        }
    }

    if (!assembly_started && !model.needs_password_setup && !model.profile_probe_failed) {
        assembly_started = true;
        // §9.1 首帧装配例外（唯一）：主线程、主循环首帧同步装配组合根；
        // 装配期间事件源尚未接通，无唤醒先于装配的竞态。窗口以 clearColor
        // 底色等待首帧（耗时登记见 aki-run.log）。
        log_line("compose: first frame — host assembly starting (data root"
                 " default resolve_data_root(), GUI host has no argv)");
        const auto started = std::chrono::steady_clock::now();
        // M5-03 跨线程唤醒接线（设计 §9.1）：executor 侧快照发布后调用
        // app::requestUpdate() 唤醒主循环重组（原子标志 + postEmptyEvent，
        // 线程安全）。首次触发留一条日志（RULE-11 证据面）。
        const auto& assembly = host.ensure_assembled(model.data_directory, [] {
            static const bool first = [] {
                log_line("wake: on_publish fired -> app::requestUpdate()");
                return true;
            }();
            (void)first;
            app::requestUpdate();
        }, std::move(model.initial_password),
            std::move(model.initial_device_name));
        aki::ui::clear_secret(model.initial_password);
        log_assembly(assembly,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started));
        if (!assembly.ok) {
            // 降级：错误占位 UI + 关窗后仍经 onShutdown 闭合（§9.1；禁
            // std::exit——不销毁自动对象，main.cpp console 先例）。
            model.startup_error = assembly.failure_reason;
        } else {
            // 消费面与出站面装配（M5-03，§9.1）：本地身份喂给消费水位面
            // （传输方向/会话端点归属判定），UiActions 绑定四 Manager 出站
            // 方法——页面经模型读取，不持有 Manager/transport 对象。
            model.state_view.local_device =
                aki::device::DeviceId{assembly.local_device_id};
            // M5-07 Settings 装配（§9.1 Settings 页与主题三选装配条款）：
            // 主题初值 = 跟随系统解析一次（平台条件编译单元，有界注册表
            // 读取；Unknown 回落 Light——页内披露）；数据目录经
            // HostRuntime::data_root() 装配面（std::string，RULE-10）。
            model.theme_setting = aki::ui::ThemeSetting::FollowSystem;
            const auto system = aki::app::query_system_theme();
            model.theme =
                aki::ui::resolve_effective_theme(model.theme_setting, system);
            model.data_directory = host.data_root();
            log_line(std::string("settings: theme follow system -> ")
                + (system == aki::ui::SystemTheme::Dark ? "dark" : "light")
                + " (system="
                + (system == aki::ui::SystemTheme::Unknown ? "unknown"
                                                           : "queried")
                + "), data directory: " + host.data_root());
            ui_actions() = std::make_shared<aki::ui::models::UiActions>(
                aki::ui::models::make_ui_actions(host.device_manager(),
                    host.conversation_manager(), host.message_manager(),
                    host.transfer_manager()));
            ui_actions()->set_local_pairing_password = [&host](
                std::string password, std::string& error) {
                return host.set_local_pairing_password(
                    std::move(password), error);
            };
            ui_actions()->set_language = [&host](std::string code) {
                return host.set_language(std::move(code));
            };
            ui_actions()->set_device_name = [&host](std::string name) {
                return host.set_device_name(std::move(name));
            };
            // M7/DEC-028：relay 注册/移除与 TURN 配置出站面绑定（同
            // set_language 先例——组合根直绑 HostRuntime，页面不持有宿主）。
            ui_actions()->enroll_relay = [&host](std::string url,
                std::string tenant, std::string token, std::string ca,
                std::string& error) {
                return host.enroll_relay(std::move(url), std::move(tenant),
                    std::move(token), std::move(ca), error);
            };
            ui_actions()->remove_relay = [&host](std::string& error) {
                return host.remove_relay(error);
            };
            ui_actions()->set_turn_server = [&host](std::string turn_host,
                unsigned port, std::string username, std::string credential,
                std::string& error) {
                return host.set_turn_server(std::move(turn_host), port,
                    std::move(username), std::move(credential), error);
            };
            // TURN 高级区预填（装配期解析的首条 turn_udp 配置；凭据不回填
            // ——保存时须重新输入）。端口留空时占位提示缺省 3478。
            const auto turn_prefill = host.turn_server();
            model.settings_turn_host_draft = turn_prefill.host;
            model.settings_turn_port_draft = turn_prefill.port == 0
                ? std::string{}
                : std::to_string(turn_prefill.port);
            model.settings_turn_username_draft = turn_prefill.username;
            if (model.language_selection_pending || assembly.identity_created) {
                (void)host.set_language(model.language
                    == aki::ui::Language::Chinese ? "zh-CN" : "en");
                model.language_selection_pending = false;
            }
            ui_actions()->open_file_folder = aki::ui::platform::open_file_folder;
            ui_actions()->copy_local_path = aki::ui::platform::copy_local_path;
            model.actions = ui_actions();
        }
        log_theme_override();
        char screen_line[128] = {};
        std::snprintf(screen_line, sizeof(screen_line),
            "compose: screen %.0fx%.0f", screen.width, screen.height);
        log_line(screen_line);
    }

    // M5-03 消费面（§9.1 快照消费与唤醒）：主线程先有界推进状态 owner
    //（pump_state：单写者 owner 上下文的有界 drain，无等待无 IO——发布后
    // 经 on_publish 回调触发 requestUpdate），再以水位去重消费快照并重派生
    // 四域视图模型；无新快照时 consume 返回 false（不阻塞、不轮询）。
    if (host.assembled()) {
        host.pump_state();
        (void)aki::ui::models::consume_ui_state(
            host.state_owner(), model.watermark, model.state_view);
    }

    // 首帧之后的 compose：只读派生页面模型（三不纪律回归）。
    aki::ui::composeMainWindow(ui, screen, model);
}

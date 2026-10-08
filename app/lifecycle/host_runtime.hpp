// HostRuntime——EUI-NEO 无关的宿主组合根（设计第 8.3 节七步 + 第 11.1 节 ②③，
// DEC-009/DEC-006；M5-02 抽离自根 main.cpp，设计 §9.1「首帧装配例外」条款）。
//
// 职责：把原 console 宿主 main.cpp 的装配/关闭编排收敛为可被两种宿主共享的
// 单元——GUI 宿主（根 main.cpp 的 dslAppConfig()+compose() 钩子）经首次
// compose 惰性装配、DslAppConfig::onShutdown 薄委托关闭；console 驱动
// （aki_host_smoke）与关闭路径测试（test_host_runtime）直接对 assemble/
// shutdown 编排验证 DOD-02 六项（测试 exe 不链 EUI-NEO，DEC-005）。
//
// 线程契约（沿 main.cpp 先例）：ensure_assembled()/shutdown_with_report()/
// quiesce() 只在主线程（owner 线程，EXEC-01）；start/stop_discovery 与快照
// 读取任意线程可调，但必须与 shutdown 串行。进程内经 instance() 共享唯一
// 实例（函数级 static 单例）——「装配成功 ⇒ 关闭必经 shutdown_with_report」
// 由 GUI 框架控制流保证（主循环一切退出路径汇入 app::shutdown() →
// onShutdown；compose 只在主循环内发生，见设计 §9.1 启动↔关闭配对条款）。
//
// 公开面（RULE-10）：仅 std/aki/executor 接线层类型（executor_owner.hpp 同款
// 口径）；无 EUI-NEO/平台类型。
#pragma once

#include "app/lifecycle/executor_owner.hpp"
#include "app/application/conversation_manager.hpp"
#include "app/application/device_manager.hpp"
#include "app/application/message_manager.hpp"
#include "app/application/transfer_manager.hpp"
#include "app/state/app_state.hpp"
#include "app/state/app_state_owner.hpp"
#include "persistence/recovery/startup_recovery.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aki::app {

// 装配证据（§8.3 七步 + §11.1 ② 恢复段计数；失败时 failure_reason 非空）。
struct HostAssemblyReport {
    bool attempted = false;
    bool ok = false;
    std::string failure_reason;
    std::size_t recovered_devices = 0;
    std::size_t recovered_conversations = 0;
    std::size_t recovered_messages = 0;
    std::size_t recovered_transfers = 0;
    std::size_t migrations_applied = 0;
    std::size_t tmp_orphans_removed = 0;
    bool identity_created = false;
    std::string local_device_id;
    bool lan_interfaces = false;
    bool name_announcement_started = false;
    std::string receive_dir;
    // peer_sessions 观察管道随装配启动（M5-11：主动/被动配对、presence、
    // 路径与断线重连的事件源；false = executor 拒绝，装配失败可见）。
    bool peer_observation_started = false;
    // M7/DEC-028：ICE 配置装配期解析证据（缺失文件 = 0/0 合法；非法行
    // 跳过计数可观测，不阻断装配）。
    std::uint64_t ice_invalid_lines = 0;
    std::size_t ice_servers_configured = 0;
};

// 启动恢复播种策略（M5-11，§8.1/§11.1；组合根公开面供单测）：恢复行中
// trust_state == Unknown 的设备是历史扫描残留（从未被用户确认，非 §8.1
// 「已知设备记录」）——不进入会话 DeviceStore，避免设备列表跨会话累积；
// 非空数据根同时在启动期检查归档文件存在性；非法记录/超预算显式失败。
// 其重新出现在网时经发现观察管道以真实存活状态再次进入。Pending（在途
// 确认）与 Trusted/Rejected/Revoked（用户决策/授权记录）原值恢复。
[[nodiscard]] AppState seeded_app_state(
    const aki::persistence::RecoveredData& data,
    const std::string& data_root = {});

// 受控关闭证据（§8.3 宿主钩子原序 + EXEC-01 步骤 2~5 + 写路径复验）。
// hook_sequence 按实际执行顺序记录钩子步名，测试据此刻画「不省略不重排」。
struct HostShutdownReport {
    bool attempted = false;
    bool hook_sequence_completed = false;
    std::vector<std::string> hook_sequence;
    ExecutorShutdownReport executor_report{};
    // 钩子各步完成标志（与 hook_sequence 对应的布尔面，供断言/日志摘要）。
    bool transfers_cancelled = false;
    bool managers_flushed = false;
    bool adapter_delivery_stopped = false;
    bool peer_pipeline_stopped = false;
    bool reconnect_futures_consumed = false;
    bool node_stopped = false;
    bool runtime_stopped = false;
    bool borrowed_runtime_shutdown_performed = false;  // 期望 false（DEC-006）
    bool runtime_drain_timed_out = false;              // 期望 false
    bool state_owner_closed = false;
    bool db_drain_requested = false;
    bool db_drained_within_budget = false;
    bool db_budget_exhausted = false;
    // 写路径复验（DEC-009 ①：admit 作业全结算、无拒绝、无失败）。
    std::uint64_t write_admitted = 0;
    std::uint64_t write_enqueue_rejected = 0;
    std::uint64_t write_settle_failures = 0;
    std::string first_write_settle_error;
    std::uint64_t db_completed = 0;
    std::uint64_t db_failed = 0;
    std::uint64_t db_rejected = 0;
    std::uint64_t post_accept_failures = 0;
    bool language_write_failed = false;
};

class HostRuntime {
public:
    // 进程内唯一实例（函数级 static：GUI compose/onShutdown 与 console 驱动共享；
    // atexit 析构兜底仅在 onShutdown 未执行的极端路径触发受控关闭）。
    static HostRuntime& instance();

    HostRuntime(const HostRuntime&) = delete;
    HostRuntime& operator=(const HostRuntime&) = delete;

    // 首帧装配（主线程；幂等——重复调用返回首次结果，两参数均被忽略）。
    // data_root 为空时缺省 resolve_data_root()（GUI 宿主：框架 main 无 argv，
    // 设计 §9.1）。wake 为快照发布唤醒回调（M5-03，设计 §9.1 跨线程唤醒
    // 接线）：转发给 AppStateOwnerOptions::on_publish——owner 上下文、
    // snapshot_.publish 之后同步调用（GUI 宿主传 app::requestUpdate()，
    // console/测试宿主传计数器或 no-op；类型 std::function<void()> EUI-NEO
    // 无关，RULE-10）。装配失败：内部执行受控回收（部分构造件按关闭序拆除），
    // assembled()==false 且 failure_reason 非空——调用方以错误占位呈现，
    // 关闭仍经 shutdown_with_report()（幂等）闭合。
    const HostAssemblyReport& ensure_assembled(std::string data_root = {},
        std::function<void()> wake = {}, std::string initial_password = {},
        std::string initial_device_name = {});

    // 主线程设置本机配对 verifier；返回 false 时 error 为可展示原因。
    [[nodiscard]] bool set_local_pairing_password(std::string password,
        std::string& error);
    [[nodiscard]] bool set_language(std::string language_code);
    // 本机设备名改名（M5-16）：与首启同一校验（1-64 字节、无控制字符），
    // 经状态 owner 字段级更新（公钥绑定校验 + DB display_name 列）并热更新
    // 局域网名称广播；广播不可用时改名仍然生效（DEC-020 尽力而为元数据）。
    [[nodiscard]] bool set_device_name(std::string name);

    // ---- M7/DEC-028：relay 注册与 TURN 配置（主线程调用，沿
    // set_language/set_device_name 先例）。静态校验失败同步 false 且
    // error 可展示；enroll_relay / enroll_relay_with_password 的网络结果
    // 异步经 RelayStatus 状态可见（SetRelayStatus）。M8-08（HEY-20261006-001
    // 收口）：注册/移除**热生效**——enroll 任务成功后与 remove 均按 profile
    // 当前记录热更新运行中 Node（NodeSession::apply_relay_enrollment_now
    // → 上游 update_relay_config，strand 投递免锁），无需重启；热更新失败
    // 经 RelayStatus.last_error 如实可见。TURN 静态配置仍重启后生效
    //（ice-servers.txt 装配期注入，设置页文案如实披露）。M8-04：密码模式
    // 为主路径（DEC-028 决策 11 阶段 2——TOFU 首连 + relay 回传指纹自动
    // 锚定；无 ca_file 时不校验链）；token 模式保留为高级路径；两种凭据
    // 均用后擦除（DEC-018 纪律）。----
    [[nodiscard]] bool enroll_relay(std::string relay_url,
        std::string tenant, std::string bootstrap_token,
        std::string ca_file, std::string& error);
    [[nodiscard]] bool enroll_relay_with_password(std::string relay_url,
        std::string enrollment_password, std::string ca_file,
        std::string& error);
    [[nodiscard]] bool remove_relay(std::string& error);
    [[nodiscard]] bool set_turn_server(std::string host, unsigned port,
        std::string username, std::string credential, std::string& error);
    // TURN 服务器预填视图（装配期解析的首条 turn_udp 配置；凭据不回填
    // ——保存时须重新输入）。
    struct TurnServerView {
        std::string host;
        unsigned port = 0;
        std::string username;
    };
    [[nodiscard]] TurnServerView turn_server() const noexcept;

    [[nodiscard]] bool assembled() const noexcept;
    [[nodiscard]] bool assembly_failed() const noexcept;
    [[nodiscard]] const HostAssemblyReport& assembly_report() const noexcept;
    [[nodiscard]] const std::string& data_root() const noexcept;

    // 观察操作面（宿主演示/驱动与测试用；与 shutdown 串行）。
    [[nodiscard]] bool start_discovery_observation();
    [[nodiscard]] bool discovery_observation_running() const;
    void stop_discovery_observation();
    // peer_sessions 观察管道运行态（M5-11：随装配启动，关闭钩子停止）。
    [[nodiscard]] bool peer_observation_running() const;
    // 每步推进到静止：flush 四 Manager（有界预算）+ 状态 owner drain 至
    // 水位不变（main.cpp 同款语义）。
    void quiesce();
    // DoubleBuffer 快照读取（槽位忙重试；presence 等易失字段原样返回）。
    [[nodiscard]] bool load_state_snapshot(AppState& out) const;

    // 受控关闭（主线程；幂等，返回首次报告）。§8.3 钩子原序 + EXEC-01 步骤
    // 2~5 + 写路径 future 逐个消费；部分装配状态同样安全（各步判存在）。
    const HostShutdownReport& shutdown_with_report();

    [[nodiscard]] bool shutdown_completed() const noexcept;
    [[nodiscard]] const HostShutdownReport& last_shutdown_report() const noexcept;

    // 宿主 executor 访问（前置 assembled；DOD-02 沿宿主生命周期路径提交任务的
    // 测试面；executor 类型属 app 接线层公开面，同 executor_owner.hpp）。
    [[nodiscard]] kairo::Executor& executor();

    // ---- M5-03 消费面/出站面装配访问器（组合根公开面；前置 assembled）----
    // 生命周期：全部指向 Impl 内成员（host 单例进程生命周期覆盖），调用方
    // 不得跨 shutdown 持有引用。线程契约沿各成员既有纪律：state_owner 的
    // drain/close 仅主线程（owner 上下文），快照/路径读取任意上下文；
    // Manager 访问器仅供组合根绑定 UiActions（ui/models，RULE-01 方向：
    // ui→application），页面不得直接持有。
    [[nodiscard]] AppStateOwner& state_owner();
    [[nodiscard]] DeviceManager& device_manager();
    [[nodiscard]] ConversationManager& conversation_manager();
    [[nodiscard]] MessageManager& message_manager();
    [[nodiscard]] TransferManager& transfer_manager();

    // 有界状态推进（主线程 = owner 上下文；compose 上下文可调用——单次
    // drain 有界：至多 64 更新/128 事件，无等待无 IO，§9.1 三不纪律的
    // 有界工作单元同款）。Manager 泵任务随时入队更新，本调用把存量推进为
    // 快照发布（发布后触发 on_publish 唤醒回调）。
    void pump_state();

private:
    HostRuntime();
    ~HostRuntime();  // 兜底：未受控关闭时按关闭序回收（禁 std::exit 先例）

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aki::app

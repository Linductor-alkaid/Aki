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
#include "app/state/app_state.hpp"

#include <cstdint>
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
    std::string receive_dir;
};

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
};

class HostRuntime {
public:
    // 进程内唯一实例（函数级 static：GUI compose/onShutdown 与 console 驱动共享；
    // atexit 析构兜底仅在 onShutdown 未执行的极端路径触发受控关闭）。
    static HostRuntime& instance();

    HostRuntime(const HostRuntime&) = delete;
    HostRuntime& operator=(const HostRuntime&) = delete;

    // 首帧装配（主线程；幂等——重复调用返回首次结果，data_root 参数被忽略）。
    // data_root 为空时缺省 resolve_data_root()（GUI 宿主：框架 main 无 argv，
    // 设计 §9.1）。装配失败：内部执行受控回收（部分构造件按关闭序拆除），
    // assembled()==false 且 failure_reason 非空——调用方以错误占位呈现，
    // 关闭仍经 shutdown_with_report()（幂等）闭合。
    const HostAssemblyReport& ensure_assembled(std::string data_root = {});

    [[nodiscard]] bool assembled() const noexcept;
    [[nodiscard]] bool assembly_failed() const noexcept;
    [[nodiscard]] const HostAssemblyReport& assembly_report() const noexcept;
    [[nodiscard]] const std::string& data_root() const noexcept;

    // 观察操作面（宿主演示/驱动与测试用；与 shutdown 串行）。
    [[nodiscard]] bool start_discovery_observation();
    [[nodiscard]] bool discovery_observation_running() const;
    void stop_discovery_observation();
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
    [[nodiscard]] executor::Executor& executor();

private:
    HostRuntime();
    ~HostRuntime();  // 兜底：未受控关闭时按关闭序回收（禁 std::exit 先例）

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aki::app

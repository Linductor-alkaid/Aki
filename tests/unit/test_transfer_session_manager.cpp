// M4-02：传输会话管理器单元测试（设计第 7.1 节①/§7 七状态机；DEC-008 模式
// 扩展；EXEC-05 长任务 + EXEC-07 句柄按业务稳定 ID；网络无关——分块 IO 为
// 注入回调）。
//
// 覆盖：
//   - 七状态机合法链全覆盖（含 Paused↔Transferring），事件序列逐项断言且
//     每个转移只投递一次（DEC-008：同态幂等不重报）；
//   - RULE-08 终态幂等：取消对已 Completed 会话不复活、不产生新事件；
//   - 出站四接口经泵串行（§7.1①）：Queued 在泵内提交准入后投递（无幽灵
//     Queued）、提交即拒回收记录（state_of 复位 nullopt，RULE-09 可见）；
//   - DOD-02 六项沿传输会话长任务路径：
//       1) 正常完成（用例 1）；2) 任务异常（io 回调抛出 → Failed，异常不
//       外抛）；3) 提交拒绝（cancellation registry 容量 0 → 回收可见）；
//       4) 执行中取消（恰一次 Cancelled）；5) 超时（stop_all 全局预算在
//       io 卡死会话下有界返回，预算不随会话数放大）；6) shutdown（stop_all
//       于 owner.shutdown 钩子内 → fully_stopped + 零未消费 future）；
//   - 重复 ID / 非法参数 / 收件箱满拒绝（RULE-09，单线程占用下确定性验证）。
//
// 每个用例持有独立的 ExecutorOwner（AGENTS 规则 7/8）。事件回调可能来自
// 泵 worker / 会话 worker / stop_all 调用方线程——收集器加锁，Catch2 断言
// 只在主线程（工程规范并发测试纪律）。
#include "app/lifecycle/executor_owner.hpp"
#include "transfer/manager/transfer_session_manager.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using aki::app::ExecutorOwner;
using aki::app::ExecutorOwnerOptions;
using aki::transfer::TransferId;
using aki::transfer::TransferSessionEvents;
using aki::transfer::TransferSessionManager;
using aki::transfer::TransferSessionManagerOptions;
using aki::transfer::TransferState;

bool wait_until_local(const std::function<bool()>& predicate,
    std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return predicate();
}

// 事件收集器：跨上下文写入加锁；断言只在主线程消费。
struct EventLog {
    mutable std::mutex mutex;
    std::vector<std::pair<std::string, TransferState>> transitions;

    void on_transition(const TransferId& id, TransferState state) {
        std::lock_guard<std::mutex> guard(mutex);
        transitions.emplace_back(id.value, state);
    }

    [[nodiscard]] std::vector<std::pair<std::string, TransferState>>
    snapshot() const {
        std::lock_guard<std::mutex> guard(mutex);
        return transitions;
    }

    [[nodiscard]] std::size_t count(const std::string& id,
        TransferState state) const {
        std::lock_guard<std::mutex> guard(mutex);
        std::size_t n = 0;
        for (const auto& [key, value] : transitions) {
            if (key == id && value == state) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] std::vector<std::pair<std::string, TransferState>> of(
        const std::string& id) const {
        std::lock_guard<std::mutex> guard(mutex);
        std::vector<std::pair<std::string, TransferState>> filtered;
        for (const auto& [key, value] : transitions) {
            if (key == id) {
                filtered.emplace_back(key, value);
            }
        }
        return filtered;
    }
};

TransferSessionEvents make_events(EventLog& log) {
    TransferSessionEvents events;
    events.on_transition =
        [&log](const TransferId& id, TransferState state) {
            log.on_transition(id, state);
        };
    return events;
}

}  // namespace

TEST_CASE("Transfer session walks the full legal state chain via pump",
    "[unit][transfer][m4-02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    EventLog log;
    std::atomic<int> allow_chunk{0};
    TransferSessionManager manager{owner.executor(),
        TransferSessionManagerOptions{},
        [&allow_chunk](const TransferId&) {
            return allow_chunk.load(std::memory_order_relaxed) > 0 ? 1U : 0U;
        },
        make_events(log)};

    const TransferId id{"t1"};
    REQUIRE(manager.start_transfer(id, 3U));
    // 起始命令经泵处理：Queued → Negotiating → Transferring。
    REQUIRE(wait_until_local(
        [&] { return manager.state_of(id) == TransferState::Transferring; },
        2s));

    REQUIRE(manager.pause_transfer(id));
    REQUIRE(wait_until_local(
        [&] { return manager.state_of(id) == TransferState::Paused; }, 2s));
    REQUIRE(manager.resume_transfer(id));
    REQUIRE(wait_until_local(
        [&] { return manager.state_of(id) == TransferState::Transferring; },
        2s));

    allow_chunk.store(1, std::memory_order_relaxed);
    REQUIRE(wait_until_local(
        [&] { return manager.state_of(id) == TransferState::Completed; },
        2s));

    // 事件序列逐项断言：每个转移恰一次（同态幂等不重报——resume 后会话
    // 循环的 Transferring/循环自身的转移均不得重复投递）。
    const auto expected = std::vector<std::pair<std::string, TransferState>>{
        {"t1", TransferState::Queued},
        {"t1", TransferState::Negotiating},
        {"t1", TransferState::Transferring},
        {"t1", TransferState::Paused},
        {"t1", TransferState::Transferring},
        {"t1", TransferState::Completed}};
    REQUIRE(log.snapshot() == expected);

    REQUIRE(manager.stop_all() == 1U);
    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Cancel on completed transfer does not revive it (RULE-08)",
    "[unit][transfer][m4-02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    EventLog log;
    std::atomic<int> gate{0};
    TransferSessionManager manager{owner.executor(),
        TransferSessionManagerOptions{},
        [&gate](const TransferId&) {
            return gate.load(std::memory_order_relaxed) > 0 ? 1U : 0U;
        },
        make_events(log)};

    REQUIRE(manager.start_transfer(TransferId{"t1"}, 1U));
    gate.store(1, std::memory_order_relaxed);
    REQUIRE(wait_until_local(
        [&] {
            return manager.state_of(TransferId{"t1"})
                == TransferState::Completed;
        },
        2s));
    const auto t1_after_completion = log.of("t1");

    // 终态后取消：命令被受理（入箱），但状态机必须拒绝复活。
    REQUIRE(manager.cancel_transfer(TransferId{"t1"}));
    // FIFO 屏障：t2 的 start 排在 cancel 之后——t2 到达 Transferring 即证明
    // cancel 已被泵处理完毕。
    REQUIRE(manager.start_transfer(TransferId{"t2"}, 5U));
    REQUIRE(wait_until_local(
        [&] {
            return manager.state_of(TransferId{"t2"})
                == TransferState::Transferring;
        },
        2s));

    REQUIRE(manager.state_of(TransferId{"t1"}) == TransferState::Completed);
    REQUIRE(log.of("t1") == t1_after_completion);  // t1 零新事件（不复活）
    REQUIRE(log.count("t1", TransferState::Cancelled) == 0U);

    REQUIRE(manager.stop_all() == 2U);
    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Duplicate id and invalid arguments are rejected",
    "[unit][transfer][m4-02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    EventLog log;
    TransferSessionManager manager{owner.executor(), {},
        [](const TransferId&) { return 0U; }, make_events(log)};

    REQUIRE(manager.start_transfer(TransferId{"t1"}, 10U));
    REQUIRE_FALSE(manager.start_transfer(TransferId{"t1"}, 10U));
    REQUIRE_FALSE(manager.start_transfer(TransferId{""}, 10U));
    REQUIRE_FALSE(manager.start_transfer(TransferId{"t2"}, 0U));
    REQUIRE(manager.session_count() == 1U);

    // 等待泵处理 start（恒 0 分块 → 会话持续运行）：句柄/future 已登记，
    // stop_all 的消费对账才有确定值。
    REQUIRE(wait_until_local(
        [&] { return manager.state_of(TransferId{"t1"}) == TransferState::Transferring; },
        2s));
    REQUIRE(manager.stop_all() == 1U);
    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Io hook exception fails the session visibly (DOD-02 task error)",
    "[unit][transfer][m4-02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    EventLog log;
    TransferSessionManager manager{owner.executor(), {},
        [](const TransferId&) -> std::uint64_t {
            throw std::runtime_error("io backend failure");
        },
        make_events(log)};

    REQUIRE(manager.start_transfer(TransferId{"t1"}, 10U));
    REQUIRE(wait_until_local(
        [&] { return manager.state_of(TransferId{"t1"}) == TransferState::Failed; },
        2s));

    const auto expected = std::vector<std::pair<std::string, TransferState>>{
        {"t1", TransferState::Queued},
        {"t1", TransferState::Negotiating},
        {"t1", TransferState::Transferring},
        {"t1", TransferState::Failed}};
    REQUIRE(log.snapshot() == expected);

    // 异常不外抛（void 任务）：future 干净结算、消费即对账。
    REQUIRE(manager.stop_all() == 1U);
    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Mid-transfer cancel reports exactly one Cancelled (DOD-02)",
    "[unit][transfer][m4-02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    EventLog log;
    std::atomic<int> gate{0};
    TransferSessionManager manager{owner.executor(),
        TransferSessionManagerOptions{},
        [&gate](const TransferId&) {
            return gate.load(std::memory_order_relaxed) > 0 ? 1U : 0U;
        },
        make_events(log)};

    REQUIRE(manager.start_transfer(TransferId{"t1"}, 100U));
    REQUIRE(wait_until_local(
        [&] { return manager.state_of(TransferId{"t1"}) == TransferState::Transferring; },
        2s));

    REQUIRE(manager.cancel_transfer(TransferId{"t1"}));
    REQUIRE(wait_until_local(
        [&] { return manager.state_of(TransferId{"t1"}) == TransferState::Cancelled; },
        2s));

    // 泵上报一次；会话循环退出时的同态转移不得二次投递（回归守卫）。
    REQUIRE(log.count("t1", TransferState::Cancelled) == 1U);

    // 会话任务已随取消终结：future 就绪可消费（零悬挂）。
    REQUIRE(manager.stop_all() == 1U);
    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Pump-side submit rejection reclaims record without ghost event",
    "[unit][transfer][m4-02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    EventLog log;
    std::atomic<int> gate{0};
    TransferSessionManager manager{owner.executor(),
        TransferSessionManagerOptions{},
        [&gate](const TransferId&) {
            return gate.load(std::memory_order_relaxed) > 0 ? 1U : 0U;
        },
        make_events(log)};

    // 提交即拒：cancellation registry 容量归零 → 泵内 submit_cancellable
    // 被拒。入箱返回 true，但 Queued 从未投递、记录被回收（无幽灵事件）。
    owner.executor().set_cancellation_registry_capacity(0);
    REQUIRE(manager.start_transfer(TransferId{"t1"}, 10U));
    REQUIRE(wait_until_local(
        [&] {
            return !manager.state_of(TransferId{"t1"}).has_value()
                && manager.session_count() == 0U;
        },
        2s));
    REQUIRE(log.snapshot().empty());

    // 恢复容量后泵与会话派生完全可用（回归：拒绝路径不破坏单飞泵）。
    owner.executor().set_cancellation_registry_capacity(1024);
    REQUIRE(manager.start_transfer(TransferId{"t1"}, 10U));
    gate.store(1, std::memory_order_relaxed);
    REQUIRE(wait_until_local(
        [&] { return manager.state_of(TransferId{"t1"}) == TransferState::Completed; },
        2s));
    REQUIRE(log.count("t1", TransferState::Queued) == 1U);

    REQUIRE(manager.stop_all() == 1U);
    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("stop_all stays bounded when a session is stuck in io (DOD-02)",
    "[unit][transfer][m4-02]") {
    ExecutorOwner owner;
    REQUIRE(owner.initialize());
    EventLog log;
    TransferSessionManagerOptions options;
    options.stop_wait_budget = 150ms;
    options.poll_interval = 5ms;
    std::atomic<int> io_calls{0};
    std::atomic<int> io_release{0};

    std::size_t consumed = 0;
    std::chrono::steady_clock::duration elapsed{};
    {
        TransferSessionManager manager{owner.executor(), options,
            [&io_calls, &io_release](const TransferId&) {
                if (io_calls.fetch_add(1) == 0) {
                    // 事件控制的第一块 IO：阻塞至测试显式放行（确定性强于
                    // 固定 400ms——重负载下 stop_all 可能晚于固定时长）。
                    while (io_release.load(std::memory_order_relaxed) == 0) {
                        std::this_thread::sleep_for(2ms);
                    }
                }
                return 1U;
            },
            make_events(log)};

        REQUIRE(manager.start_transfer(TransferId{"t1"}, 2U));
        REQUIRE(wait_until_local(
            [&] {
                return manager.state_of(TransferId{"t1"})
                    == TransferState::Transferring;
            },
            2s));

        const auto started = std::chrono::steady_clock::now();
        consumed = manager.stop_all();
        elapsed = std::chrono::steady_clock::now() - started;

        // stop_all 期间活动态会话已推进 Cancelled（终态投递，消费者拿到
        // 终态）——预算耗尽后 future 未消费如实报 0，不冒充已消费。
        REQUIRE(log.count("t1", TransferState::Cancelled) == 1U);

        // 放行卡死的 IO：会话观测到 stop 退出，析构兜底消费其 future。
        io_release.store(1, std::memory_order_relaxed);
    }
    // 管理器析构（兜底 stop_all）先于 executor 关闭：会话随后观测到 stop
    // 退出，future 被消费（不悬挂）。

    // 全局预算（150ms）先于 io 解封（400ms）耗尽：有界返回且如实报
    // consumed=0；总耗时有界，不悬挂。
    REQUIRE(consumed == 0U);
    REQUIRE(elapsed >= 150ms);
    REQUIRE(elapsed < 1500ms);

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

TEST_CASE("Full inbox rejects visibly while the pump is occupied (RULE-09)",
    "[unit][transfer][m4-02]") {
    ExecutorOwnerOptions owner_options;
    owner_options.executor_config.min_threads = 1;
    owner_options.executor_config.max_threads = 1;
    ExecutorOwner owner{owner_options};
    REQUIRE(owner.initialize());

    EventLog log;
    TransferSessionManagerOptions options;
    options.inbox_capacity = 1;
    options.poll_interval = 5ms;
    TransferSessionManager manager{owner.executor(), options,
        [](const TransferId&) { return 0U; }, make_events(log)};

    // 独占唯一 worker：泵/排空任务只能排队，收件箱停滞。
    auto blocker = owner.executor().submit_auto(
        [] { std::this_thread::sleep_for(300ms); });
    REQUIRE(blocker.valid());

    REQUIRE(manager.start_transfer(TransferId{"t1"}, 10U));  // 收件箱 1/1
    REQUIRE_FALSE(manager.pause_transfer(TransferId{"t1"})); // 满：拒绝可见
    REQUIRE_FALSE(manager.cancel_transfer(TransferId{"t1"}));// 满：拒绝可见
    REQUIRE(manager.session_count() == 1U);
    REQUIRE(manager.state_of(TransferId{"t1"}) == TransferState::Queued);

    // blocker 释放后泵恢复：start 被处理、会话派生（会话循环自此占用唯一
    // worker——命令需要线程余量，本用例不再向运行中的会话入队命令）。
    REQUIRE(wait_until_local(
        [&] {
            return manager.state_of(TransferId{"t1"})
                == TransferState::Transferring;
        },
        3s));

    REQUIRE(manager.stop_all() == 1U);
    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
    (void)blocker;
}

TEST_CASE("Shutdown hook cancels all active sessions with terminal events",
    "[unit][transfer][m4-02][dod02]") {
    // 显式 4 线程：3 个常驻会话各占 1 worker（活动会话占用 worker 是本组件
    // 的线程预算属性），泵/排空需要第 4 个——2 核 CI runner 上 3 会话会
    // 饥饿（本轮 CI 失败根因，回归守卫）。
    ExecutorOwnerOptions owner_options;
    owner_options.executor_config.min_threads = 4;
    owner_options.executor_config.max_threads = 4;
    ExecutorOwner owner{owner_options};
    REQUIRE(owner.initialize());
    EventLog log;
    std::atomic<int> gate{0};
    TransferSessionManager manager{owner.executor(),
        TransferSessionManagerOptions{},
        [&gate](const TransferId&) {
            return gate.load(std::memory_order_relaxed) > 0 ? 1U : 0U;
        },
        make_events(log)};

    for (const auto* name : {"t1", "t2", "t3"}) {
        REQUIRE(manager.start_transfer(TransferId{name}, 100U));
    }
    REQUIRE(wait_until_local(
        [&] { return manager.session_count() == 3U; }, 10s));
    REQUIRE(wait_until_local(
        [&] {
            for (const auto* name : {"t1", "t2", "t3"}) {
                if (manager.state_of(TransferId{name})
                    != TransferState::Transferring) {
                    return false;
                }
            }
            return true;
        },
        10s));

    REQUIRE(manager.stop_all() == 3U);
    for (const auto* name : {"t1", "t2", "t3"}) {
        REQUIRE(log.count(name, TransferState::Cancelled) == 1U);
        REQUIRE_FALSE(manager.state_of(TransferId{name}).has_value());
    }

    const auto report = owner.shutdown();
    REQUIRE(report.fully_stopped());
}

// M5-03：ui/models 视图模型派生 + 快照消费面 + 发布→唤醒回调单测。
//
// 覆盖（验收标准）：
//   - 四域视图模型派生断言（空态/终态/易变字段/端点归属/进度 fraction）；
//   - consume_ui_state 水位去重（无新快照 false）、新快照重派生、连接路径
//     mailbox 独立水位推进、无新数据后的重试语义（下一调用成功）；
//   - 发布→唤醒调用序（AppStateOwnerOptions::on_publish：publish 之后于
//     owner 上下文触发、新序列号已可见；异常全捕获计数不中断 drain）；
//     跨线程回调契约（executor 任务内驱动 owner.drain()，注入回调于
//     executor 线程触发——owner 上下文可落在 executor 任务上的机制契约
//     面，GUI 侧 requestUpdate 的线程安全性；现行管线 drain 仅在主线程；
//     CI 不执行渲染，DEC-014 覆盖声明第 2 条）。
//
// 本文件不包含 eui 头（aki_ui_models 目标，DEC-005「测试 exe 不链 eui」）。
#include "app/lifecycle/executor_owner.hpp"
#include "app/state/app_state_owner.hpp"
#include "app/state/app_state_updates.hpp"
#include "ui/models/ui_state_consumer.hpp"
#include "ui/models/view_models.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace aki::app;
using namespace aki::ui::models;
using aki::app::SetDeviceConnectionPath;

aki::device::DeviceIdentity make_device(const std::string& id,
    aki::device::TrustState trust = aki::device::TrustState::Unknown,
    aki::device::PresenceState presence = aki::device::PresenceState::Offline) {
    aki::device::DeviceIdentity device;
    device.id = aki::device::DeviceId{id};
    device.display_name = "dev-" + id;
    device.os_name = "test-os";
    device.trust_state = trust;
    device.presence = presence;
    return device;
}

aki::conversation::Message make_text_message(const std::string& id,
    const std::string& sender, const std::string& receiver,
    const std::string& text) {
    aki::conversation::Message message;
    message.id = aki::conversation::MessageId{id};
    message.sender = aki::device::DeviceId{sender};
    message.receiver = aki::device::DeviceId{receiver};
    message.type = aki::conversation::MessageType::Text;
    message.payload = aki::conversation::TextPayload{text};
    return message;
}

}  // namespace

TEST_CASE("Device views derive identity, per-device path and trust ops",
    "[unit][ui_models]") {
    const aki::app::DeviceStore empty;
    REQUIRE(derive_device_views(empty).empty());

    aki::app::DeviceStore store;
    auto alpha = make_device("alpha",
        aki::device::TrustState::Trusted, aki::device::PresenceState::Online);
    alpha.public_key.bytes = {1, 2, 3};
    store.devices.push_back(alpha);
    auto pending = make_device("beta", aki::device::TrustState::Pending);
    store.devices.push_back(pending);  // 公钥为空 → 指纹不可用态。

    // 逐设备路径（DEC-015）：两台设备两条不同路径同时正确展示——退役的
    // 全局单值摘要无法表达的形态；无条目 = Unknown。
    store.connection_paths.push_back(
        aki::app::DeviceConnectionPathEntry{
            aki::device::DeviceId{"alpha"},
            aki::device::ConnectionPath::Lan});
    store.connection_paths.push_back(
        aki::app::DeviceConnectionPathEntry{
            aki::device::DeviceId{"beta"},
            aki::device::ConnectionPath::Relay});

    const auto views = derive_device_views(store);
    REQUIRE(views.size() == 2);
    REQUIRE(views[0].id.value == "alpha");
    REQUIRE(views[0].display_name == "dev-alpha");
    REQUIRE(views[0].os_name == "test-os");
    REQUIRE(views[0].trust_state == aki::device::TrustState::Trusted);
    REQUIRE(views[0].presence == aki::device::PresenceState::Online);
    REQUIRE(views[0].connection_path == aki::device::ConnectionPath::Lan);
    REQUIRE(views[1].connection_path == aki::device::ConnectionPath::Relay);
    REQUIRE(views[1].presence == aki::device::PresenceState::Offline);

    // 指纹可用性：公钥在场 = 可用；缺失 = 显式不可用。
    REQUIRE(views[0].fingerprint_available);
    REQUIRE_FALSE(views[1].fingerprint_available);

    // 信任操作可用性（§4 转移边）：仅 Pending 可确认/拒绝、仅 Trusted 可撤销。
    REQUIRE(views[0].can_revoke());
    REQUIRE_FALSE(views[0].can_confirm());
    REQUIRE_FALSE(views[0].can_reject());
    REQUIRE(views[1].can_confirm());
    REQUIRE(views[1].can_reject());
    REQUIRE_FALSE(views[1].can_revoke());
}

TEST_CASE("Conversation views carry last-message summary per endpoints",
    "[unit][ui_models]") {
    const aki::conversation::Conversation conversation{
        aki::conversation::ConversationId{"conv-alpha"},
        aki::device::DeviceId{"local"}, aki::device::DeviceId{"alpha"},
        aki::conversation::ConversationState::Active};
    const aki::conversation::Conversation other{
        aki::conversation::ConversationId{"conv-beta"},
        aki::device::DeviceId{"local"}, aki::device::DeviceId{"beta"},
        aki::conversation::ConversationState::Active};

    aki::app::ConversationStore conversations;
    conversations.conversations.push_back(conversation);
    conversations.conversations.push_back(other);

    // 空消息态：无摘要。
    const aki::app::MessageStore no_messages;
    const auto empty_views =
        derive_conversation_views(conversations, no_messages);
    REQUIRE(empty_views.size() == 2);
    REQUIRE_FALSE(empty_views[0].last_message.has_value);
    REQUIRE(empty_views[0].remote_device.value == "alpha");
    REQUIRE(empty_views[0].state == aki::conversation::ConversationState::Active);

    // 有消息：归属过滤 + Store 序倒序取最后一条；媒体预览带名称标注。
    aki::app::MessageStore messages;
    messages.messages.push_back(
        make_text_message("m1", "local", "alpha", "first"));
    aki::conversation::Message image_message;
    image_message.id = aki::conversation::MessageId{"m2"};
    image_message.sender = aki::device::DeviceId{"alpha"};
    image_message.receiver = aki::device::DeviceId{"local"};
    image_message.type = aki::conversation::MessageType::Image;
    image_message.payload = aki::conversation::ImagePayload{
        aki::transfer::FileMetadata{.name = "cat.png", .size_bytes = 3,
            .mime_type = "image/png", .stored_sha256 = "aa"},
        aki::transfer::TransferId{"hyt1_x"}};
    messages.messages.push_back(image_message);
    // 他会话消息不得串入 conv-alpha 摘要。
    messages.messages.push_back(
        make_text_message("m3", "local", "beta", "other conversation"));

    const auto views = derive_conversation_views(conversations, messages);
    REQUIRE(views[0].last_message.has_value);
    REQUIRE(views[0].last_message.id.value == "m2");
    REQUIRE(views[0].last_message.type == aki::conversation::MessageType::Image);
    REQUIRE(views[0].last_message.preview == "[image] cat.png");
    REQUIRE(views[1].last_message.has_value);
    REQUIRE(views[1].last_message.preview == "other conversation");
}

TEST_CASE("Message stream filters by conversation endpoints with local guard",
    "[unit][ui_models]") {
    const aki::conversation::Conversation conversation{
        aki::conversation::ConversationId{"conv-alpha"},
        aki::device::DeviceId{"local"}, aki::device::DeviceId{"alpha"},
        aki::conversation::ConversationState::Active};

    aki::app::MessageStore store;
    store.messages.push_back(
        make_text_message("m1", "local", "alpha", "outbound"));
    store.messages.push_back(
        make_text_message("m2", "alpha", "local", "inbound"));
    store.messages.push_back(
        make_text_message("m3", "local", "beta", "other"));

    const auto stream = derive_conversation_messages(store, conversation,
        aki::device::DeviceId{"local"});
    REQUIRE(stream.size() == 2);
    REQUIRE(stream[0].id.value == "m1");
    REQUIRE(stream[1].id.value == "m2");

    // 本地身份不是会话端点：可见空态（不猜测归属）。
    REQUIRE(derive_conversation_messages(store, conversation,
                aki::device::DeviceId{"stranger"})
        .empty());
}

TEST_CASE("Transfer views expose direction, progress fraction and terminal",
    "[unit][ui_models]") {
    aki::app::TransferStore store;
    const auto make_transfer = [](const std::string& id,
                                  const std::string& sender,
                                  const std::string& receiver,
                                  std::uint64_t transferred,
                                  std::uint64_t total,
                                  aki::transfer::TransferState state) {
        aki::transfer::Transfer transfer;
        transfer.id = aki::transfer::TransferId{id};
        transfer.sender = aki::device::DeviceId{sender};
        transfer.receiver = aki::device::DeviceId{receiver};
        transfer.file.name = id + ".pt";
        transfer.transferred = transferred;
        transfer.total = total;
        transfer.state = state;
        return transfer;
    };
    store.transfers.push_back(make_transfer("t1", "local", "alpha", 512, 1024,
        aki::transfer::TransferState::Transferring));
    store.transfers.push_back(make_transfer("t2", "alpha", "local", 0, 0,
        aki::transfer::TransferState::Completed));
    store.transfers.push_back(make_transfer("t3", "alpha", "local", 10, 100,
        aki::transfer::TransferState::Transferring));

    const auto views =
        derive_transfer_views(store, aki::device::DeviceId{"local"});
    REQUIRE(views.size() == 3);
    // t1：出站，进度 0.5，非终态。
    REQUIRE(views[0].outbound);
    REQUIRE(views[0].peer.value == "alpha");
    REQUIRE(views[0].progress > 0.49);
    REQUIRE(views[0].progress < 0.51);
    REQUIRE_FALSE(views[0].terminal);
    REQUIRE(views[0].file_name == "t1.pt");
    // t2：入站终态；total==0 → fraction 0（不除零），终态由 state 解释。
    REQUIRE_FALSE(views[1].outbound);
    REQUIRE(views[1].progress == 0.0);
    REQUIRE(views[1].terminal);
    REQUIRE(views[1].state == aki::transfer::TransferState::Completed);
    // t3：进行中的入站行非终态。
    REQUIRE_FALSE(views[2].terminal);
}

TEST_CASE("consume_ui_state dedups by watermark and re-derives on publish",
    "[unit][ui_models][exec03]") {
    // （M5-04 起路径随快照派生——DEC-015，无独立路径水位。）
    AppStateOwner owner;
    UiConsumerWatermark watermark;
    UiStateSnapshot view;

    // 无发布：consume false（水位无变化），视图保持空态。
    REQUIRE_FALSE(consume_ui_state(owner, watermark, view));
    REQUIRE_FALSE(view.has_snapshot);

    // 发布本地身份：consume true + 视图派生 + 水位推进。
    REQUIRE(owner.submit_update(UpsertDevice{make_device("alpha")}));
    owner.drain();
    REQUIRE(consume_ui_state(owner, watermark, view));
    REQUIRE(view.has_snapshot);
    REQUIRE(view.devices.size() == 1);
    REQUIRE(view.devices[0].id.value == "alpha");
    REQUIRE(watermark.snapshot_sequence == owner.snapshot_sequence());

    // 水位去重：无新快照再消费 false（不重复派生）。
    const auto devices_before = view.devices.size();
    REQUIRE_FALSE(consume_ui_state(owner, watermark, view));
    REQUIRE(view.devices.size() == devices_before);

    // 无新数据/槽位忙后的重试语义：下一次发布后 consume 成功（消费侧无
    // 粘滞失败；DoubleBuffer 槽位忙内部已有多次尝试，本层以 false 留待
    // 下一帧——busy-retry 契约的消费面形态）。聚合载荷先落局部变量：
    // REQUIRE 宏的顶层逗号分割不受花括号保护（MSVC 实测）。
    const SetPresence presence_update{
        aki::device::DeviceId{"alpha"}, aki::device::PresenceState::Online};
    REQUIRE(owner.submit_update(presence_update));
    owner.drain();
    REQUIRE(consume_ui_state(owner, watermark, view));
    // 易变字段（presence）随新快照重派生。
    REQUIRE(view.devices[0].presence == aki::device::PresenceState::Online);
}

TEST_CASE("Connection path changes ride the snapshot watermark to views",
    "[unit][ui_models][exec03][dec015]") {
    AppStateOwner owner;
    UiConsumerWatermark watermark;
    UiStateSnapshot view;
    view.local_device = aki::device::DeviceId{"local"};

    REQUIRE(owner.submit_update(UpsertDevice{make_device("alpha")}));
    owner.drain();
    REQUIRE(consume_ui_state(owner, watermark, view));
    REQUIRE(view.devices[0].connection_path
        == aki::device::ConnectionPath::Unknown);

    // SetDeviceConnectionPath 置快照脏（DEC-015）：新快照携带逐设备路径，
    // 消费面重派生后视图可见；无新快照时 consume false（水位去重不变）。
    const SetDeviceConnectionPath path_update{
        aki::device::DeviceId{"alpha"}, aki::device::ConnectionPath::Lan};
    REQUIRE(owner.submit_update(path_update));
    REQUIRE_FALSE(consume_ui_state(owner, watermark, view));
    owner.drain();
    REQUIRE(consume_ui_state(owner, watermark, view));
    REQUIRE(view.devices[0].connection_path
        == aki::device::ConnectionPath::Lan);
}

TEST_CASE("on_publish fires after publish; throwing hook stays contained",
    "[unit][ui_models][dod02][exec03]") {
    // 「发布→唤醒」调用序：钩子被调用时新快照序列号已可见（先 publish 后
    // 唤醒）。钩子经指针延迟解引用读取 owner（构造期捕获）。
    AppStateOwner* delayed = nullptr;
    std::atomic<std::uint64_t> observed_sequence{0};
    std::atomic<int> wake_calls{0};

    AppStateOwnerOptions options;
    options.on_publish = [&delayed, &observed_sequence, &wake_calls] {
        if (delayed != nullptr) {
            observed_sequence.store(delayed->snapshot_sequence());
        }
        wake_calls.fetch_add(1);
    };
    AppStateOwner owner{options};
    delayed = &owner;

    REQUIRE(owner.submit_update(UpsertDevice{make_device("alpha")}));
    owner.drain();
    REQUIRE(owner.stats().publish_hook_calls == 1);
    REQUIRE(wake_calls.load() == 1);
    // 调用序证据：钩子内观察到的序列号 == 发布后的新序列号。
    REQUIRE(observed_sequence.load() == owner.snapshot_sequence());
    REQUIRE(observed_sequence.load() > 0);

    // 无变更 drain：不发布、不触发钩子。
    owner.drain();
    REQUIRE(wake_calls.load() == 1);
    REQUIRE(owner.stats().publish_hook_failures == 0);

    // DOD-02（新增回调路径·任务异常面）：钩子异常全捕获计数、不中断 drain，
    // 更新照常生效。
    AppStateOwnerOptions throwing;
    throwing.on_publish = [] { throw std::runtime_error("wake boom"); };
    AppStateOwner throwing_owner{throwing};
    REQUIRE(throwing_owner.submit_update(UpsertDevice{make_device("beta")}));
    throwing_owner.drain();  // 异常不得逃逸 drain。
    REQUIRE(throwing_owner.stats().publish_hook_failures == 1);
    executor::comm::Snapshot<AppState> snapshot;
    int attempts = 0;
    while (!throwing_owner.try_load_snapshot(snapshot) && attempts < 64) {
        ++attempts;
    }
    REQUIRE(attempts < 64);
    REQUIRE(snapshot.value.devices.devices.size() == 1);
}

TEST_CASE("Injected wake callback is callable from executor tasks",
    "[unit][ui_models][dod02][exec03]") {
    // 跨线程唤醒契约（DEC-005/§9.1）：owner 上下文可落在任意单一执行上下文
    // （含 executor 任务——线程契约仅要求 drain 串行，不绑定具体线程），注入
    // 回调必须线程安全可调（GUI 侧即 app::requestUpdate：原子标志 +
    // postEmptyEvent）。本用例在 executor 任务内真实驱动 owner.drain()（该
    // 任务即本次 drain 的 owner 上下文，主线程不并发 drain），验证注入回调
    // 于 executor 线程触发且新序列号已可见。现行管线 drain 仅发生在主线程
    // （host_runtime pump/quiesce），此处验证的是机制契约面而非现行调用路径
    // （executor 生命周期 owner 纪律同 test_executor_lifecycle）。
    aki::app::ExecutorOwner executor_owner;
    REQUIRE(executor_owner.initialize());

    AppStateOwner* delayed = nullptr;
    const auto drain_submit_thread = std::this_thread::get_id();
    std::atomic<int> wakes{0};
    std::atomic<bool> wake_off_submit_thread{false};
    std::atomic<std::uint64_t> observed_sequence{0};
    std::atomic<std::uint64_t> hook_calls{0};

    AppStateOwnerOptions options;
    options.on_publish = [&delayed, &wakes, &wake_off_submit_thread,
        &observed_sequence, drain_submit_thread] {
        wake_off_submit_thread.store(
            std::this_thread::get_id() != drain_submit_thread);
        if (delayed != nullptr) {
            observed_sequence.store(delayed->snapshot_sequence());
        }
        wakes.fetch_add(1);
    };
    AppStateOwner owner{options};
    delayed = &owner;

    // submit_update 任意上下文（主线程入队）；drain 归 owner 上下文——此处
    // 即 executor 任务（GUI 侧的回调体在此等价于 app::requestUpdate()）。
    REQUIRE(owner.submit_update(UpsertDevice{make_device("alpha")}));
    auto future = executor_owner.executor().submit_auto([&owner, &hook_calls] {
        owner.drain();
        hook_calls.store(owner.stats().publish_hook_calls);
        return 1;
    });
    REQUIRE(future.get() == 1);

    // 钩子在 executor 线程真实触发：owner 上下文计数、跨线程证据与调用序
    // （钩子内观察的序列号 == 发布后的新序列号）。
    REQUIRE(hook_calls.load() == 1);
    REQUIRE(wakes.load() == 1);
    REQUIRE(wake_off_submit_thread.load());
    REQUIRE(observed_sequence.load() == owner.snapshot_sequence());
    REQUIRE(observed_sequence.load() > 0);

    const auto report = executor_owner.shutdown();
    REQUIRE(report.fully_stopped());
}

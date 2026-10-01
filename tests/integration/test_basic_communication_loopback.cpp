// M5-33/34: fresh identities, one Executor owner, no network skip exits.
#include "app/application/reconnect_loop.hpp"
#include "app/application/router_sink.hpp"
#include "app/lifecycle/executor_owner.hpp"
#include "heyaki/adapter/heyaki_node_adapter.hpp"
#include "heyaki/adapter/lan_discovery.hpp"
#include "heyaki/adapter/peer_sessions_pipeline.hpp"
#include "heyaki/session/runtime_node.hpp"
#include "persistence/storage/sha256.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <optional>
#include <span>
#include <thread>

namespace {
using namespace std::chrono_literals;
using aki::heyaki::LocalProfile;
using aki::heyaki::NodeSession;
using aki::device::DeviceId;

bool await(const std::function<bool()>& predicate, std::chrono::milliseconds budget = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

std::string fresh_root() {
    return (std::filesystem::temp_directory_path() / ("aki-basic-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()))).string();
}

struct Pair {
    std::string root = fresh_root();
    LocalProfile a_profile = LocalProfile::open(root + "/a", "a-password");
    LocalProfile b_profile = LocalProfile::open(root + "/b", "b-password");
    aki::app::ExecutorOwner owner;
    std::optional<NodeSession> a;
    std::optional<NodeSession> b;
    DeviceId a_id = a_profile.identity().id;
    DeviceId b_id = b_profile.identity().id;

    Pair(bool a_basic, bool b_basic, std::chrono::milliseconds offer_timeout = 0ms,
         bool b_receive_root = true) {
        if (!owner.initialize()) throw std::runtime_error("executor admission failed");
        std::filesystem::create_directories(root + "/a/inbox");
        std::filesystem::create_directories(root + "/b/inbox");
        a.emplace(NodeSession::create(owner.executor(), {.profile = &a_profile,
            .worker_name = "basic-a",
            .file_receive_roots = {{.name = "inbox", .directory = root + "/a/inbox"}},
            .basic_communication = a_basic,
            .file_offer_timeout = offer_timeout}));
        b.emplace(NodeSession::create(owner.executor(), {.profile = &b_profile,
            .worker_name = "basic-b",
            .file_receive_roots = b_receive_root
                ? std::vector<::heyaki::FileRootConfig>{{.name = "inbox", .directory = root + "/b/inbox"}}
                : std::vector<::heyaki::FileRootConfig>{},
            .basic_communication = b_basic,
            .file_offer_timeout = offer_timeout}));
    }
    ~Pair() {
        if (a) (void)a->shutdown();
        if (b) (void)b->shutdown();
        (void)owner.shutdown();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void connect(std::string_view stage = "initial") {
        INFO("Connection stage: " << stage);
        REQUIRE(a->has_lan_interfaces());
        REQUIRE(b->has_lan_interfaces());
        REQUIRE(await([&] { return a->endpoint_visible(b_id) && b->endpoint_visible(a_id); }));
        REQUIRE(a->connect_lan(b_id));
        REQUIRE(await([&] { return a->session_linked(b_id) && b->session_linked(a_id); }));
    }
    void no_grants() {
        const auto ad = a->trust_grants(b_id);
        const auto bd = b->trust_grants(a_id);
        REQUIRE(ad.has_value()); REQUIRE(bd.has_value());
        REQUIRE_FALSE(ad->issued); REQUIRE_FALSE(ad->received);
        REQUIRE_FALSE(bd->issued); REQUIRE_FALSE(bd->received);
    }
};

// Product managers and adapter, sharing the pair's single Executor owner.
// State is drained by the test thread, matching the host's snapshot boundary.
struct ApplicationSide {
    aki::app::AppStateOwner state;
    aki::heyaki::HeyakiNodeAdapter adapter;
    aki::app::DeviceManager devices;
    aki::app::ConversationManager conversations;
    aki::app::MessageManager messages;
    aki::app::TransferManager transfers;
    aki::app::RouterSink router;
    aki::heyaki::PeerSessionPipeline peers;

    ApplicationSide(executor::Executor& executor, LocalProfile& profile, NodeSession& session)
        : adapter(executor,
                  {.profile = &profile,
                   .session = &session,
                   .conversation_for =
                       [](const DeviceId& remote) {
                           return aki::conversation::ConversationId{"conv-" + remote.value};
                       },
                   .peer_observation = false}),
          devices(executor, state, adapter), conversations(executor, state),
          messages(executor, state, adapter, {.local_device = session.local_id()}),
          transfers(executor, state, adapter), router(devices, conversations, messages, transfers),
          peers(
              executor, session,
              {.on_connected = [this](const auto& peer,
                                      auto path) { (void)router.on_device_connected(peer, path); },
               .on_disconnected =
                   [this](const auto& peer) { (void)router.on_device_disconnected(peer); },
               .on_connection_path_changed =
                   [this](const auto& peer, auto path) {
                       (void)router.on_connection_path_changed(
                           peer, aki::device::ConnectionPath::Unknown, path);
                   },
               .on_pairing_ready =
                   [this, &session](const auto& peer) {
                       for (const auto& endpoint : session.endpoints()) {
                           if (endpoint.device_id == peer) {
                               (void)router.on_pairing_ready(peer, endpoint.public_key);
                               break;
                           }
                       }
                   },
               .on_trust_changed =
                   [this](const auto& peer) { (void)devices.enqueue_trust_calibration(peer); }}) {
        adapter.set_sink(&router);
    }
    void settle() {
        if (!devices.flush(2s) || !conversations.flush(2s) || !messages.flush(2s) ||
            !transfers.flush(2s)) {
            throw std::runtime_error("application manager flush failed");
        }
        state.drain();
    }
    aki::app::AppState snapshot() {
        settle();
        executor::comm::Snapshot<aki::app::AppState> snapshot;
        if (!state.try_load_snapshot(snapshot))
            throw std::runtime_error("snapshot unavailable");
        return snapshot.value;
    }
};

struct ShutdownGuard {
    std::function<void()> stop;
    ~ShutdownGuard() {
        if (!stop) return;
        try { stop(); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "test cleanup failed: %s\n", error.what());
        }
    }
};

std::string hash_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(in), {}};
    return aki::persistence::sha256_hex(std::as_bytes(std::span{bytes}));
}
} // namespace

TEST_CASE("Untrusted devices exchange text image and inbox files both ways", "[integration][basic_communication]") {
    // Counters outlive the sessions and their callback teardown, including failures.
    std::atomic<unsigned> received_a{0}, received_b{0}, acked_a{0}, acked_b{0};
    std::atomic<unsigned> committed_a{0}, committed_b{0};
    std::atomic<unsigned> connected{0}, disconnected{0}, discovered{0};
    Pair pair(true, true);
    pair.connect(); pair.no_grants();
    aki::heyaki::LanDiscoveryPipeline discovery(pair.owner.executor(), *pair.a,
        [&](const auto&) { ++discovered; });
    aki::heyaki::PeerSessionEvents events;
    events.on_connected = [&](const auto&, auto) { ++connected; };
    events.on_disconnected = [&](const auto&) { ++disconnected; };
    aki::heyaki::PeerSessionPipeline peers(pair.owner.executor(), *pair.a, std::move(events));
    // Even a failed assertion must drain callbacks while both pipelines and
    // their observer owners are alive. Timer cancellation alone is insufficient.
    ShutdownGuard shutdown{[&] {
        discovery.stop(); peers.stop();
        if (pair.a) (void)pair.a->shutdown();
        if (pair.b) (void)pair.b->shutdown();
        (void)pair.owner.shutdown();
    }};
    REQUIRE(discovery.start(10ms));
    REQUIRE(peers.start(10ms));
    REQUIRE(await([&] { return discovered >= 1 && connected == 1; }));
    for (const auto* session : {&*pair.a, &*pair.b}) {
        const auto views = session->peer_session_views();
        REQUIRE(views.size() == 1);
        REQUIRE(views.front().pairing_restricted);
        REQUIRE(views.front().basic_communication);
        REQUIRE(views.front().authorized_scopes.empty());
        auto scopes = views.front().policy_scopes;
        std::sort(scopes.begin(), scopes.end());
        REQUIRE(scopes == std::vector<std::string>{"file.push:inbox", "message.send"});
    }
    auto handlers = [](NodeSession& session, std::atomic<unsigned>& inbound, std::atomic<unsigned>& acked) {
        session.set_message_handlers([&inbound](const auto&, const auto&, const auto& type, const auto& payload) {
            if ((type == "aki.text" && payload == "basic text") || type == "aki.image" || type == "aki.file") ++inbound;
        }, [&acked](const auto&, const auto&, const std::string& event) { if (event == "acked") ++acked; });
    };
    handlers(*pair.a, received_a, acked_a); handlers(*pair.b, received_b, acked_b);
    const auto a_transfer = NodeSession::new_transfer_id();
    const auto b_transfer = NodeSession::new_transfer_id();
    const auto source = std::filesystem::path(pair.root) / "payload.bin";
    { std::ofstream out(source, std::ios::binary); out << std::string(4096, 'f'); }
    const auto hash = hash_file(source);
    aki::transfer::FileMetadata image{"photo.png", 4096, "image/png", hash};
    aki::transfer::FileMetadata file{"payload.bin", 4096, "application/octet-stream", hash};
    REQUIRE(pair.a->send_text(pair.b_id, aki::heyaki::new_message_id(), "basic text"));
    REQUIRE(pair.b->send_text(pair.a_id, aki::heyaki::new_message_id(), "basic text"));
    REQUIRE(pair.a->send_image(pair.b_id, aki::heyaki::new_message_id(), image, a_transfer));
    REQUIRE(pair.b->send_image(pair.a_id, aki::heyaki::new_message_id(), image, b_transfer));
    REQUIRE(pair.a->send_file(pair.b_id, aki::heyaki::new_message_id(), file, a_transfer));
    REQUIRE(pair.b->send_file(pair.a_id, aki::heyaki::new_message_id(), file, b_transfer));
    REQUIRE(await([&] { return received_a == 3 && received_b == 3 && acked_a == 3 && acked_b == 3; }, 30s));
    pair.a->set_file_event_observer([&](const auto&, const NodeSession::FileTransferEventView& event) {
        if (event.phase == static_cast<int>(::heyaki::FileTransferPhase::committed)) ++committed_a;
    });
    pair.b->set_file_event_observer([&](const auto&, const NodeSession::FileTransferEventView& event) {
        if (event.phase == static_cast<int>(::heyaki::FileTransferPhase::committed)) ++committed_b;
    });
    REQUIRE(pair.a->push_file(pair.b_id, "inbox", "payload.bin", source, a_transfer));
    REQUIRE(pair.b->push_file(pair.a_id, "inbox", "photo.png", source, b_transfer));
    REQUIRE(await([&] { return committed_a >= 1 && committed_b >= 1 &&
        std::filesystem::exists(pair.root + "/a/inbox/photo.png") &&
        std::filesystem::exists(pair.root + "/b/inbox/payload.bin"); }, 30s));
    REQUIRE(hash_file(pair.root + "/a/inbox/photo.png") == hash);
    REQUIRE(hash_file(pair.root + "/b/inbox/payload.bin") == hash);
    pair.no_grants();
    REQUIRE(pair.b->shutdown().node_stopped);
    REQUIRE(await([&] { return !pair.a->session_linked(pair.b_id) && disconnected >= 1; }));
    // Wait for the old announcement lease to expire before issuing one dial.
    // Production retries stale directory candidates through ReconnectCoordinator.
    REQUIRE(await([&] { return !pair.a->endpoint_visible(pair.b_id); }));
    REQUIRE_FALSE(pair.a->send_text(pair.b_id, aki::heyaki::new_message_id(), "basic text"));
    pair.b.reset();
    pair.b.emplace(NodeSession::create(pair.owner.executor(), {.profile = &pair.b_profile,
        .worker_name = "basic-b-restarted",
        .file_receive_roots = {{.name = "inbox", .directory = pair.root + "/b/inbox"}},
        .basic_communication = true}));
    handlers(*pair.b, received_b, acked_b);
    pair.connect("restarted peer");
    REQUIRE(await([&] { return connected == 2; }));
    REQUIRE(pair.a->send_text(pair.b_id, aki::heyaki::new_message_id(), "basic text"));
    REQUIRE(await([&] { return received_b == 4 && acked_a == 4; }));
    pair.no_grants();
    discovery.stop(); peers.stop();
    REQUIRE_FALSE(discovery.running()); REQUIRE_FALSE(peers.running());
    const auto a_report = pair.a->shutdown(); const auto b_report = pair.b->shutdown();
    REQUIRE(a_report.node_stopped); REQUIRE(b_report.node_stopped);
    REQUIRE(a_report.runtime_stopped); REQUIRE(b_report.runtime_stopped);
    REQUIRE_FALSE(a_report.runtime_executor_shutdown_performed);
    REQUIRE_FALSE(b_report.runtime_executor_shutdown_performed);
    REQUIRE(pair.owner.shutdown().fully_stopped());
    shutdown.stop = {};
}

TEST_CASE("One-sided basic policy refuses untrusted traffic explicitly", "[integration][basic_communication]") {
    std::atomic<unsigned> received{0}, failed{0}, file_terminal{0}, file_committed{0}, file_paused{0}, file_offered{0};
    Pair pair(true, false, 700ms); pair.connect(); pair.no_grants();
    pair.b->set_message_handlers([&](const auto&, const auto&, const auto&, const auto&) { ++received; }, {});
    pair.a->set_message_handlers({}, [&](const auto&, const auto&, const std::string& event) { if (event == "failed") ++failed; });
    const bool admitted = pair.a->send_text(pair.b_id, aki::heyaki::new_message_id(), "basic text");
    REQUIRE((!admitted || await([&] { return failed > 0; }, 40s)));
    REQUIRE(received == 0);
    REQUIRE_FALSE(pair.b->send_text(pair.a_id, aki::heyaki::new_message_id(), "basic text"));
    pair.a->set_file_event_observer([&](const auto&, const NodeSession::FileTransferEventView& event) {
        const auto phase = static_cast<::heyaki::FileTransferPhase>(event.phase);
        if (phase == ::heyaki::FileTransferPhase::committed) ++file_committed;
        if (phase == ::heyaki::FileTransferPhase::paused) ++file_paused;
        if (phase == ::heyaki::FileTransferPhase::offered) ++file_offered;
        if (phase == ::heyaki::FileTransferPhase::failed || phase == ::heyaki::FileTransferPhase::cancelled) ++file_terminal;
    });
    const auto source = std::filesystem::path(pair.root) / "blocked.bin";
    { std::ofstream out(source, std::ios::binary); out << "blocked bytes"; }
    const auto transfer = NodeSession::new_transfer_id();
    const bool file_admitted = pair.a->push_file(pair.b_id, "inbox", "blocked.bin", source, transfer);
    REQUIRE(file_admitted);
    // HEY-20261002-001: refusal can retire the cached business channel; the upstream known
    // follow-up then loses this session and parks a subsequent push. Session
    // loss is Paused by contract, not an offer-expiry terminal. Both outcomes
    // must be observable within the budget, never an endless offered state.
    const bool resolved = await([&] { return file_terminal == 1 || file_paused > 0; }, 5s);
    INFO("post-message refusal: terminal=" << file_terminal.load()
        << " paused=" << file_paused.load() << " offered=" << file_offered.load()
        << " linked=" << pair.a->session_linked(pair.b_id));
    REQUIRE(resolved);
    if (file_terminal == 0) {
        REQUIRE(file_paused > 0);
        // The paused callback precedes publication of the failed session snapshot.
        REQUIRE(await([&] { return !pair.a->session_linked(pair.b_id); }, 5s));
    }
    // Expired and session-parked IDs both refuse this later cancellation.
    // This does not claim that cancelling a parked transfer is implemented.
    REQUIRE_FALSE(pair.a->cancel_file_transfer(pair.b_id, transfer));
    REQUIRE(file_committed == 0);
    REQUIRE_FALSE(std::filesystem::exists(pair.root + "/b/inbox/blocked.bin"));
    pair.no_grants();
    REQUIRE(pair.a->shutdown().node_stopped);
    REQUIRE(pair.b->shutdown().node_stopped);
    REQUIRE(pair.owner.shutdown().fully_stopped());
    REQUIRE(file_terminal <= 1);
}

TEST_CASE("Unanswered file offers produce one failure without grants or disk effects", "[integration][basic_communication][file_offer]") {
    std::atomic<unsigned> failed{0}, cancelled{0}, committed{0}, wrong_id{0};
    std::atomic<bool> offer_expired{false};
    const bool strict_peer = GENERATE(true, false);
    Pair pair(true, !strict_peer, 700ms, strict_peer);
    pair.connect(); pair.no_grants();
    const auto transfer = NodeSession::new_transfer_id();
    pair.a->set_file_event_observer([&, transfer](const auto&, const NodeSession::FileTransferEventView& event) {
        if (event.transfer != transfer) ++wrong_id;
        const auto phase = static_cast<::heyaki::FileTransferPhase>(event.phase);
        if (phase == ::heyaki::FileTransferPhase::failed) {
            offer_expired = event.error == "offer_expired";
            ++failed;
        }
        if (phase == ::heyaki::FileTransferPhase::cancelled) ++cancelled;
        if (phase == ::heyaki::FileTransferPhase::committed) ++committed;
    });
    const auto source = std::filesystem::path(pair.root) / "refused.bin";
    { std::ofstream out(source, std::ios::binary); out << "refused payload"; }
    REQUIRE(pair.a->push_file(pair.b_id, "inbox", "refused.bin", source, transfer));
    REQUIRE(await([&] { return failed == 1; }, 5s));
    if (strict_peer) REQUIRE(offer_expired.load());
    REQUIRE_FALSE(pair.a->cancel_file_transfer(pair.b_id, transfer));
    pair.no_grants();
    REQUIRE_FALSE(std::filesystem::exists(pair.root + "/b/inbox/refused.bin"));
    REQUIRE(pair.a->shutdown().node_stopped);
    REQUIRE(pair.b->shutdown().node_stopped);
    REQUIRE(pair.owner.shutdown().fully_stopped());
    REQUIRE(failed == 1);
    REQUIRE(cancelled == 0);
    REQUIRE(committed == 0);
    REQUIRE(wrong_id == 0);
}

TEST_CASE("Cancelling an unanswered offer wins before expiry with one terminal", "[integration][basic_communication][file_offer]") {
    std::atomic<unsigned> offered{0}, failed{0}, cancelled{0}, committed{0};
    Pair pair(true, false, 5s);
    pair.connect(); pair.no_grants();
    const auto transfer = NodeSession::new_transfer_id();
    pair.a->set_file_event_observer([&](const auto&, const NodeSession::FileTransferEventView& event) {
        const auto phase = static_cast<::heyaki::FileTransferPhase>(event.phase);
        if (phase == ::heyaki::FileTransferPhase::offered) ++offered;
        if (phase == ::heyaki::FileTransferPhase::failed) ++failed;
        if (phase == ::heyaki::FileTransferPhase::cancelled) ++cancelled;
        if (phase == ::heyaki::FileTransferPhase::committed) ++committed;
    });
    const auto source = std::filesystem::path(pair.root) / "cancelled.bin";
    { std::ofstream out(source, std::ios::binary); out << "cancelled payload"; }
    REQUIRE(pair.a->push_file(pair.b_id, "inbox", "cancelled.bin", source, transfer));
    REQUIRE(await([&] { return offered > 0; }));
    REQUIRE(pair.a->cancel_file_transfer(pair.b_id, transfer));
    REQUIRE(await([&] { return cancelled == 1; }));
    REQUIRE_FALSE(pair.a->cancel_file_transfer(pair.b_id, transfer));
    pair.no_grants();
    REQUIRE_FALSE(std::filesystem::exists(pair.root + "/b/inbox/cancelled.bin"));
    REQUIRE(pair.a->shutdown().node_stopped);
    REQUIRE(pair.b->shutdown().node_stopped);
    REQUIRE(pair.owner.shutdown().fully_stopped());
    REQUIRE(cancelled == 1);
    REQUIRE(failed == 0);
    REQUIRE(committed == 0);
}

TEST_CASE("Password grants can be repaired in both directions on an authorized session", "[integration][pairing]") {
    std::atomic<unsigned> a_success{0}, b_success{0}, b_failure{0};
    Pair pair(false, false); pair.connect(); pair.no_grants();
    pair.a->set_pairing_observer([&](const DeviceId&, bool ok, const auto&) { if (ok) ++a_success; });
    pair.b->set_pairing_observer([&](const DeviceId&, bool ok, const auto&) { if (ok) ++b_success; else ++b_failure; });
    REQUIRE(pair.a->pair_peer(pair.b_id, "b-password"));
    REQUIRE(await([&] { return a_success == 1 && pair.a->session_authenticated(pair.b_id) && pair.b->session_authenticated(pair.a_id); }, 25s));
    REQUIRE(pair.a->trust_grants(pair.b_id)->received);
    REQUIRE(pair.b->trust_grants(pair.a_id)->issued);
    REQUIRE(pair.b->pair_peer(pair.a_id, "a-password"));
    REQUIRE(await([&] { return b_success == 1; }, 25s));
    REQUIRE(pair.a->trust_grants(pair.b_id)->issued);
    REQUIRE(pair.b->trust_grants(pair.a_id)->received);
    REQUIRE(pair.b->pair_peer(pair.a_id, "wrong-password"));
    REQUIRE(await([&] { return b_failure == 1; }, 25s));
    REQUIRE(pair.a->session_authenticated(pair.b_id));
    REQUIRE(pair.b->session_authenticated(pair.a_id));
}

TEST_CASE("Fresh discovery opens untrusted conversations and calibrates mutual grants",
          "[integration][basic_communication][application_path][m541]") {
    Pair pair(true, true);
    ApplicationSide a(pair.owner.executor(), pair.a_profile, *pair.a);
    ApplicationSide b(pair.owner.executor(), pair.b_profile, *pair.b);
    ShutdownGuard shutdown{[&] {
        a.peers.stop();
        b.peers.stop();
        (void)pair.a->shutdown();
        (void)pair.b->shutdown();
        a.settle();
        b.settle();
        a.state.close();
        b.state.close();
        (void)pair.owner.shutdown();
    }};
    REQUIRE(await([&] {
        return pair.a->endpoint_visible(pair.b_id) && pair.b->endpoint_visible(pair.a_id);
    }));
    aki::heyaki::LanDiscoveryState discovery_state;
    const auto discovered = aki::heyaki::diff_lan_discovery(pair.a->endpoints(), discovery_state);
    const auto peer = std::find_if(discovered.discovered.begin(), discovered.discovered.end(),
        [&](const auto& entry) { return entry.identity.id == pair.b_id; });
    REQUIRE(peer != discovered.discovered.end());
    REQUIRE(a.router.on_device_discovered(*peer));
    auto snapshot = a.snapshot();
    REQUIRE(snapshot.devices.devices.size() == 1);
    REQUIRE(snapshot.devices.devices.front().trust_state == aki::device::TrustState::Unknown);
    // This is the same connection selection predicate used by the host sweep.
    REQUIRE(aki::app::should_reconnect_known_device(snapshot.devices.devices.front(), pair.a_id));
    REQUIRE(pair.a->connect_lan(pair.b_id));
    REQUIRE(a.peers.start(20ms));
    REQUIRE(b.peers.start(20ms));
    auto linked_in_ui = [](const aki::app::AppState& state) {
        return !state.devices.connection_paths.empty() &&
               state.devices.connection_paths.front().path != aki::device::ConnectionPath::Unknown;
    };
    REQUIRE(await([&] { return linked_in_ui(a.snapshot()) && linked_in_ui(b.snapshot()); }));
    pair.no_grants();
    REQUIRE(a.conversations.ensure_conversation(pair.a_id, pair.b_id));
    REQUIRE(a.snapshot().conversations.conversations.front().state ==
            aki::conversation::ConversationState::Active);
    REQUIRE(b.snapshot().conversations.conversations.empty());
    REQUIRE(a.messages.send_text(pair.b_id, aki::heyaki::new_message_id(), "untrusted a"));
    REQUIRE(await([&] { return b.snapshot().messages.messages.size() == 1; }, 3s));
    REQUIRE(b.snapshot().conversations.conversations.size() == 1);
    REQUIRE(b.messages.send_text(pair.a_id, aki::heyaki::new_message_id(), "untrusted b"));
    auto delivered = [](const aki::app::AppState& state) {
        return state.messages.messages.size() == 2 &&
               std::all_of(state.messages.messages.begin(), state.messages.messages.end(),
                           [](const auto& message) {
                               return message.state == aki::conversation::DeliveryState::Delivered;
                           });
    };
    REQUIRE(await([&] { return delivered(a.snapshot()) && delivered(b.snapshot()); }, 30s));
    pair.no_grants();
    auto trust = [](const aki::app::AppState& state) {
        return std::pair{state.devices.devices.front().trust_state,
                         state.devices.devices.front().inbound_trust};
    };
    REQUIRE(trust(a.snapshot()).first != aki::device::TrustState::Trusted);
    REQUIRE_FALSE(trust(a.snapshot()).second);
    REQUIRE_FALSE(trust(b.snapshot()).second);
    REQUIRE(a.devices.confirm_pairing(pair.b_id, "b-password"));
    REQUIRE(await(
        [&] {
            return trust(a.snapshot()).first == aki::device::TrustState::Trusted &&
                   trust(b.snapshot()).second;
        },
        25s));
    REQUIRE_FALSE(trust(a.snapshot()).second);
    REQUIRE(trust(b.snapshot()).first != aki::device::TrustState::Trusted);
    // Reverse pairing does not change the already authorized connection state.
    REQUIRE(b.devices.confirm_pairing(pair.a_id, "a-password"));
    REQUIRE(await(
        [&] {
            return trust(a.snapshot()).first == aki::device::TrustState::Trusted &&
                   trust(a.snapshot()).second &&
                   trust(b.snapshot()).first == aki::device::TrustState::Trusted &&
                   trust(b.snapshot()).second;
        },
        25s));
    REQUIRE(linked_in_ui(a.snapshot()));
    REQUIRE(linked_in_ui(b.snapshot()));
}

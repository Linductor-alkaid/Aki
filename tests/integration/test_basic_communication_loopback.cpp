// M5-33/34: fresh identities, one Executor owner, no network skip exits.
#include "app/lifecycle/executor_owner.hpp"
#include "heyaki/session/runtime_node.hpp"
#include "persistence/storage/sha256.hpp"

#include <catch2/catch_test_macros.hpp>
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

    Pair(bool a_basic, bool b_basic) {
        if (!owner.initialize()) throw std::runtime_error("executor admission failed");
        std::filesystem::create_directories(root + "/a/inbox");
        std::filesystem::create_directories(root + "/b/inbox");
        a.emplace(NodeSession::create(owner.executor(), {.profile = &a_profile,
            .worker_name = "basic-a",
            .file_receive_roots = {{.name = "inbox", .directory = root + "/a/inbox"}},
            .basic_communication = a_basic}));
        b.emplace(NodeSession::create(owner.executor(), {.profile = &b_profile,
            .worker_name = "basic-b",
            .file_receive_roots = {{.name = "inbox", .directory = root + "/b/inbox"}},
            .basic_communication = b_basic}));
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
    Pair pair(true, true);
    pair.connect(); pair.no_grants();
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
    REQUIRE(await([&] { return !pair.a->session_linked(pair.b_id); }));
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
    REQUIRE(pair.a->send_text(pair.b_id, aki::heyaki::new_message_id(), "basic text"));
    REQUIRE(await([&] { return received_b == 4 && acked_a == 4; }));
    pair.no_grants();
    const auto a_report = pair.a->shutdown(); const auto b_report = pair.b->shutdown();
    REQUIRE(a_report.node_stopped); REQUIRE(b_report.node_stopped);
    REQUIRE(a_report.runtime_stopped); REQUIRE(b_report.runtime_stopped);
    REQUIRE_FALSE(a_report.runtime_executor_shutdown_performed);
    REQUIRE_FALSE(b_report.runtime_executor_shutdown_performed);
    REQUIRE(pair.owner.shutdown().fully_stopped());
}

TEST_CASE("One-sided basic policy refuses untrusted traffic explicitly", "[integration][basic_communication]") {
    std::atomic<unsigned> received{0}, failed{0}, file_terminal{0}, file_committed{0};
    Pair pair(true, false); pair.connect(); pair.no_grants();
    pair.b->set_message_handlers([&](const auto&, const auto&, const auto&, const auto&) { ++received; }, {});
    pair.a->set_message_handlers({}, [&](const auto&, const auto&, const std::string& event) { if (event == "failed") ++failed; });
    const bool admitted = pair.a->send_text(pair.b_id, aki::heyaki::new_message_id(), "basic text");
    REQUIRE((!admitted || await([&] { return failed > 0; }, 40s)));
    REQUIRE(received == 0);
    REQUIRE_FALSE(pair.b->send_text(pair.a_id, aki::heyaki::new_message_id(), "basic text"));
    pair.a->set_file_event_observer([&](const auto&, const NodeSession::FileTransferEventView& event) {
        const auto phase = static_cast<::heyaki::FileTransferPhase>(event.phase);
        if (phase == ::heyaki::FileTransferPhase::committed) ++file_committed;
        if (phase == ::heyaki::FileTransferPhase::failed || phase == ::heyaki::FileTransferPhase::cancelled) ++file_terminal;
    });
    const auto source = std::filesystem::path(pair.root) / "blocked.bin";
    { std::ofstream out(source, std::ios::binary); out << "blocked bytes"; }
    const auto transfer = NodeSession::new_transfer_id();
    const bool file_admitted = pair.a->push_file(pair.b_id, "inbox", "blocked.bin", source, transfer);
    if (file_admitted) {
        const bool resolved = await([&] { return file_terminal > 0; }, 3s);
        INFO("One-sided file refusal terminal observed: " << resolved);
        // HEY-20261001-001: no bounded rejection outcome in v1.1.1.
        // Verify refusal has no disk effects and cancellation remains available.
        std::printf("[policy-refusal] file admitted=1 terminal-before-cancel=%d\n", resolved);
        const bool cancellation_admitted = pair.a->cancel_file_transfer(pair.b_id, transfer);
        std::printf("[policy-refusal] cancellation-admitted=%d\n", cancellation_admitted);
        if (cancellation_admitted) REQUIRE(await([&] { return file_terminal > 0; }));
        // Rejection is an observable API result; v1.1.1 may already have
        // retired the violating channel without resolving its file observer.
        // This known hole remains open in HEY-20261001-001, not counted as fixed.
    }
    REQUIRE(file_committed == 0);
    REQUIRE_FALSE(std::filesystem::exists(pair.root + "/b/inbox/blocked.bin"));
    pair.no_grants();
    REQUIRE(pair.a->shutdown().node_stopped);
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

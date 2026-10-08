// Signed Aki device-name exchange over a separate LAN multicast channel.
// Socket work is nonblocking and bounded per Executor timer tick.
#pragma once

#include "heyaki/adapter/lan_name_protocol.hpp"

#include <kairo/executor.hpp>
#include <kairo/timer.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace aki::heyaki {

class LanNameBeacon {
public:
    using Sink = std::function<bool(SignedLanName)>;

    LanNameBeacon(kairo::Executor& executor,
        ::heyaki::IdentityKeyPair identity, std::string name, Sink sink)
        : executor_(executor), identity_(std::move(identity)),
          name_(std::move(name)), sink_(std::move(sink)) {}

    LanNameBeacon(const LanNameBeacon&) = delete;
    LanNameBeacon& operator=(const LanNameBeacon&) = delete;
    ~LanNameBeacon() { stop(); }

    [[nodiscard]] bool start() {
        std::lock_guard guard(mutex_);
        if (timer_.valid() || !valid_lan_name(name_)) return false;
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
        wsa_started_ = true;
#endif
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == kInvalidSocket) { close_locked(); return false; }
        int reuse = 1;
        if (setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR,
                reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0) {
            close_locked(); return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(kPort);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(socket_, reinterpret_cast<sockaddr*>(&local),
                sizeof(local)) != 0) { close_locked(); return false; }
        ip_mreq membership{};
        // inet_addr is deprecated on Windows (C4996 there is an error);
        // inet_pton is the supported replacement on both platforms.
        if (::inet_pton(AF_INET, kGroup, &group_) != 1) {
            close_locked();
            return false;
        }
        membership.imr_multiaddr = group_;
        membership.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                reinterpret_cast<const char*>(&membership),
                sizeof(membership)) != 0) { close_locked(); return false; }
#ifdef _WIN32
        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) != 0) {
            close_locked(); return false;
        }
#else
        if (fcntl(socket_, F_SETFL, fcntl(socket_, F_GETFL, 0) | O_NONBLOCK)
            != 0) { close_locked(); return false; }
#endif
        timer_ = executor_.submit_periodic(1000,
            [this] { tick(); });
        if (!timer_.valid()) { close_locked(); return false; }
        return true;
    }

    void stop() {
        std::lock_guard guard(mutex_);
        if (timer_.valid()) {
            (void)timer_.cancel();
            timer_ = kairo::TimerHandle{};
        }
        close_locked();
    }

    // Settings rename support: subsequent ticks announce the new name. The
    // announcement itself stays best-effort metadata (DEC-020).
    [[nodiscard]] bool set_name(std::string name) {
        std::lock_guard guard(mutex_);
        if (!valid_lan_name(name)) return false;
        name_ = std::move(name);
        return true;
    }

private:
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
    using Socket = int;
    static constexpr Socket kInvalidSocket = -1;
#endif
    static constexpr std::uint16_t kPort = 49191;
    static constexpr const char* kGroup = "239.255.42.98";

    // Resolved once in start(); valid whenever socket_ is open.
    in_addr group_{};

    void tick() {
        std::lock_guard guard(mutex_);  // timer callbacks may overlap.
        if (socket_ == kInvalidSocket) return;
        auto packet = encode_lan_name(identity_, name_);
        if (!packet.empty()) {
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(kPort);
        remote.sin_addr = group_;
            (void)::sendto(socket_, reinterpret_cast<const char*>(packet.data()),
                static_cast<int>(packet.size()), 0,
                reinterpret_cast<sockaddr*>(&remote), sizeof(remote));
        }
        std::array<std::byte, 256> bytes{};
        for (int i = 0; i < 32; ++i) {
#ifdef _WIN32
            const int count = ::recv(socket_,
                reinterpret_cast<char*>(bytes.data()),
                static_cast<int>(bytes.size()), 0);
#else
            const int count = static_cast<int>(::recv(socket_, bytes.data(),
                bytes.size(), 0));
#endif
            if (count <= 0) break;
            auto peer = decode_lan_name(std::span<const std::byte>(bytes.data(),
                static_cast<std::size_t>(count)));
            if (peer && peer->id.value != ::heyaki::to_string(identity_.device_id()))
                (void)sink_(std::move(*peer));
        }
    }

    void close_locked() {
        if (socket_ != kInvalidSocket) {
#ifdef _WIN32
            closesocket(socket_);
#else
            ::close(socket_);
#endif
            socket_ = kInvalidSocket;
        }
#ifdef _WIN32
        if (wsa_started_) { WSACleanup(); wsa_started_ = false; }
#endif
    }

    kairo::Executor& executor_;
    ::heyaki::IdentityKeyPair identity_;
    std::string name_;
    Sink sink_;
    std::mutex mutex_;
    Socket socket_ = kInvalidSocket;
    kairo::TimerHandle timer_;
#ifdef _WIN32
    bool wsa_started_ = false;
#endif
};

}  // namespace aki::heyaki

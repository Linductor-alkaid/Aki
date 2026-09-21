// 设备身份领域类型（设计第 3 节）。M1 固定契约，密码学表示在 M3 对齐 DEC-006。
#pragma once

#include "device/trust/trust_state.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aki::device {

// 稳定设备标识，取值来自 Heyaki 身份体系（M3 前保持不透明字符串包装）。
struct DeviceId {
    std::string value;

    bool empty() const noexcept { return value.empty(); }
    friend bool operator==(const DeviceId&, const DeviceId&) = default;
};

struct PublicKey {
    std::vector<std::uint8_t> bytes;

    friend bool operator==(const PublicKey&, const PublicKey&) = default;
};

// 设备类别（设计第 2/3 节：电脑、手机、服务器、机器人等）。
enum class DeviceClass : std::uint8_t {
    Desktop,
    Tablet,
    Phone,
    Server,
    Robot,
    Other,
};

// 在线状态（设计第 3 节）。由 Device Manager 依据 presence 事件维护，无转移约束。
enum class PresenceState : std::uint8_t {
    Online,
    Offline,
};

// 当前连接路径（设计第 5 节，SCOPE-10）。路径变化不改变 Conversation 与消息历史。
enum class ConnectionPath : std::uint8_t {
    Unknown,
    Lan,
    P2p,
    Relay,
};

constexpr std::string_view to_string(PresenceState state) noexcept {
    switch (state) {
        case PresenceState::Online: return "Online";
        case PresenceState::Offline: return "Offline";
    }
    return "Unknown";
}

constexpr std::string_view to_string(ConnectionPath path) noexcept {
    switch (path) {
        case ConnectionPath::Unknown: return "Unknown";
        case ConnectionPath::Lan: return "LAN";
        case ConnectionPath::P2p: return "P2P";
        case ConnectionPath::Relay: return "Relay";
    }
    return "Unknown";
}

// 能力声明（设计第 12 节）。声明能力不代表获得授权，permission 判定独立于信任。
struct DeviceCapabilities {
    bool messaging = false;
    bool file_transfer = false;
    bool status_query = false;
    bool command_execution = false;
    bool remote_terminal = false;
    bool agent = false;

    friend bool operator==(const DeviceCapabilities&, const DeviceCapabilities&) = default;
};

struct DeviceIdentity {
    DeviceId id;
    std::string display_name;
    DeviceClass device_class = DeviceClass::Other;
    std::string os_name;
    PublicKey public_key;
    DeviceCapabilities capabilities;
    TrustState trust_state = TrustState::Unknown;
    PresenceState presence = PresenceState::Offline;

    friend bool operator==(const DeviceIdentity&, const DeviceIdentity&) = default;
};

} // namespace aki::device

// 设备发现领域类型（设计第 4 节）。不同发现来源统一表示为 DiscoveredDevice。
#pragma once

#include "device/device/device_types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace aki::device {

enum class DiscoveryMethod : std::uint8_t {
    LanDiscovery,
    KnownDevice,
    Relay,
    InviteLink,
    Manual,
};

constexpr std::string_view to_string(DiscoveryMethod method) noexcept {
    switch (method) {
        case DiscoveryMethod::LanDiscovery: return "LanDiscovery";
        case DiscoveryMethod::KnownDevice: return "KnownDevice";
        case DiscoveryMethod::Relay: return "Relay";
        case DiscoveryMethod::InviteLink: return "InviteLink";
        case DiscoveryMethod::Manual: return "Manual";
    }
    return "Unknown";
}

// 连接端点信息。M1 保持不透明；具体表示随 M3 DEC-006 对齐 Heyaki 契约后细化。
struct EndpointInfo {
    std::string description;

    friend bool operator==(const EndpointInfo&, const EndpointInfo&) = default;
};

struct DiscoveredDevice {
    DeviceIdentity identity;
    DiscoveryMethod method = DiscoveryMethod::LanDiscovery;
    EndpointInfo endpoint;

    friend bool operator==(const DiscoveredDevice&, const DiscoveredDevice&) = default;
};

} // namespace aki::device

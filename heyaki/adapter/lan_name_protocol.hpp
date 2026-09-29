// Aki LAN name announcement. Heyaki owns peer identity and discovery; this
// signed, bounded metadata packet only augments a discovered identity.
#pragma once

#include "device/device/device_types.hpp"

#include <heyaki/identity.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <utility>

namespace aki::heyaki {

struct SignedLanName {
    aki::device::DeviceId id;
    aki::device::PublicKey public_key;
    std::string name;
};

inline constexpr std::array<std::byte, 8> kLanNameMagic{
    std::byte{'A'}, std::byte{'K'}, std::byte{'I'}, std::byte{'N'},
    std::byte{'A'}, std::byte{'M'}, std::byte{'E'}, std::byte{'1'}};

[[nodiscard]] inline bool valid_lan_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

[[nodiscard]] inline std::vector<std::byte> encode_lan_name(
    const ::heyaki::IdentityKeyPair& identity, const std::string& name,
    std::chrono::system_clock::time_point now =
        std::chrono::system_clock::now()) {
    if (!valid_lan_name(name)) return {};
    std::vector<std::byte> packet;
    packet.reserve(8 + 32 + 8 + 1 + name.size() + 64);
    packet.insert(packet.end(), kLanNameMagic.begin(), kLanNameMagic.end());
    packet.insert(packet.end(), identity.public_key().begin(),
        identity.public_key().end());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    for (int shift = 56; shift >= 0; shift -= 8) {
        packet.push_back(std::byte{static_cast<unsigned char>(
            static_cast<std::uint64_t>(seconds) >> shift)});
    }
    packet.push_back(std::byte{static_cast<unsigned char>(name.size())});
    for (unsigned char c : name) packet.push_back(std::byte{c});
    auto signature = ::heyaki::sign_identity_message(identity, packet);
    if (!signature) return {};
    packet.insert(packet.end(), signature.value_if()->begin(),
        signature.value_if()->end());
    return packet;
}

[[nodiscard]] inline std::optional<SignedLanName> decode_lan_name(
    std::span<const std::byte> packet,
    std::chrono::system_clock::time_point now =
        std::chrono::system_clock::now()) {
    constexpr std::size_t kHeader = 8 + 32 + 8 + 1;
    if (packet.size() < kHeader + 1 + 64 || packet.size() > kHeader + 64 + 64)
        return std::nullopt;
    if (!std::equal(kLanNameMagic.begin(), kLanNameMagic.end(),
            packet.begin())) return std::nullopt;
    const auto length = std::to_integer<unsigned char>(packet[kHeader - 1]);
    if (packet.size() != kHeader + length + 64) return std::nullopt;
    std::uint64_t seconds = 0;
    for (std::size_t i = 40; i < 48; ++i)
        seconds = (seconds << 8) | std::to_integer<unsigned char>(packet[i]);
    const auto current = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    if (current < 0) return std::nullopt;
    const auto current_seconds = static_cast<std::uint64_t>(current);
    if (seconds > current_seconds
            ? seconds - current_seconds > 30
            : current_seconds - seconds > 30) return std::nullopt;
    std::string name;
    name.reserve(length);
    for (std::size_t i = kHeader; i < kHeader + length; ++i)
        name.push_back(static_cast<char>(std::to_integer<unsigned char>(packet[i])));
    if (!valid_lan_name(name)) return std::nullopt;
    const auto key = packet.subspan(8, 32);
    if (!::heyaki::verify_identity_signature(key,
            packet.first(kHeader + length), packet.last(64))) return std::nullopt;
    auto derived = ::heyaki::derive_device_id(key);
    if (!derived) return std::nullopt;
    SignedLanName result;
    result.id = aki::device::DeviceId{::heyaki::to_string(*derived.value_if())};
    for (std::byte byte : key)
        result.public_key.bytes.push_back(std::to_integer<std::uint8_t>(byte));
    result.name = std::move(name);
    return result;
}

}  // namespace aki::heyaki

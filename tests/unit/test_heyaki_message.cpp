// M3-05（评审拆分）：MessageId 双射与 aki.text 信封往返（DEC-006 冻结；
// 网络无关）。
//
// 独立 unit 二进制的原因（工程规范第 7 节：skip 是诊断结果，不是成功证据）：
// test_message_loopback 的 [skip] 降级路径以受控退出（_Exit）结束进程，若同
// 二进制内存在先行失败的用例，其回归会被 exit 0 掩盖。本文件的断言与网络彻底
// 解耦，始终完整执行并以自身退出码报告。
#include "heyaki/message.hpp"
#include "heyaki/session/runtime_node.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>

namespace {

::heyaki::MessageId fixed_wire_id() {
    return ::heyaki::MessageId(::heyaki::MessageId::Storage{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
        std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c},
        std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}, std::byte{0x10}});
}

}  // namespace

TEST_CASE("MessageId bijection and aki.text envelope round-trip",
    "[unit][heyaki_message]") {
    // 双射：任意 16 字节 → to_string → parse → 同一字节序（DEC-006 冻结）。
    const ::heyaki::MessageId wire = fixed_wire_id();
    const auto aki_id =
        aki::heyaki::NodeSession::to_aki_message_id(wire);
    REQUIRE(aki_id.value.rfind("hym1_", 0) == 0);  // 规范前缀
    const auto back =
        aki::heyaki::NodeSession::to_heyaki_message_id(aki_id);
    REQUIRE(back.has_value());
    REQUIRE(*back == wire);

    // 非规范形式（裸 hex / 任意串）解码失败 → nullopt（admission 拒绝可见）。
    REQUIRE_FALSE(
        aki::heyaki::NodeSession::to_heyaki_message_id(
            aki::conversation::MessageId{"m-1"})
            .has_value());
    REQUIRE_FALSE(
        aki::heyaki::NodeSession::to_heyaki_message_id(
            aki::conversation::MessageId{"0123456789abcdef0123456789abcdef"})
            .has_value());

    // aki.text 信封：encode → parse 往返（type/mode/payload 无损）。
    ::heyaki::MessageEnvelope envelope;
    envelope.message_id = wire;
    envelope.type = "aki.text";
    envelope.delivery_mode = ::heyaki::MessageDeliveryMode::peer_acked;
    const std::string text = "hello from aki";
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    envelope.payload.assign(data, data + text.size());
    auto encoded = ::heyaki::encode_message_envelope(envelope);
    REQUIRE(encoded.has_value());
    auto parsed = ::heyaki::parse_message_envelope(
        std::span<const std::byte>(
            encoded.value_if()->data(), encoded.value_if()->size()));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value_if()->message_id == wire);
    REQUIRE(parsed.value_if()->type == "aki.text");
    REQUIRE(parsed.value_if()->delivery_mode
        == ::heyaki::MessageDeliveryMode::peer_acked);
    REQUIRE(parsed.value_if()->payload.size() == text.size());
}

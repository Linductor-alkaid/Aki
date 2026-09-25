// M3-05（评审拆分）：MessageId 双射与 aki.text 信封往返（DEC-006 冻结；
// 网络无关）。
//
// 独立 unit 二进制的原因（工程规范第 7 节：skip 是诊断结果，不是成功证据）：
// test_message_loopback 的 [skip] 降级路径以受控退出（_Exit）结束进程，若同
// 二进制内存在先行失败的用例，其回归会被 exit 0 掩盖。本文件的断言与网络彻底
// 解耦，始终完整执行并以自身退出码报告。
// M4-03 增补：TransferId 双射（hyt1_ 规范串，DEC-010）与 aki.image 信封 +
// ImagePayload 冻结字段号编码往返（envelope 层，网络无关；codec 拒收路径的
// 逐项覆盖见 test_image_payload_codec）。
#include "conversation/codec/image_payload_codec.hpp"
#include "heyaki/message.hpp"
#include "heyaki/session/runtime_node.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

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

TEST_CASE("TransferId bijection and aki.image envelope round-trip",
    "[unit][heyaki_message]") {
    // 双射（DEC-010：TransferId 与 heyaki Identifier 的规范串双射）：任意
    // 16 字节 → to_string → parse → 同一字节序。
    const ::heyaki::TransferId wire_transfer(::heyaki::TransferId::Storage{
        std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24},
        std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28},
        std::byte{0x29}, std::byte{0x2a}, std::byte{0x2b}, std::byte{0x2c},
        std::byte{0x2d}, std::byte{0x2e}, std::byte{0x2f}, std::byte{0x30}});
    const aki::transfer::TransferId aki_transfer{
        ::heyaki::to_string(wire_transfer)};
    REQUIRE(aki_transfer.value.rfind("hyt1_", 0) == 0);  // 规范前缀
    REQUIRE(aki_transfer.value.size()
        == aki::conversation::codec::kImageTransferIdEncodedBytes);
    // codec 规范谓词接受 heyaki 编码器产物（谓词接受集 == 编码器像集的
    // 抽样交叉验证，DEC-010①）。
    REQUIRE(aki::conversation::codec::is_canonical_transfer_id_text(
        aki_transfer.value));
    const auto back =
        aki::heyaki::NodeSession::to_heyaki_transfer_id(aki_transfer);
    REQUIRE(back.has_value());
    REQUIRE(*back == wire_transfer);

    // 非规范形式（错误前缀 / 错误长度 / 大写 / 非法字符 / 尾部填充位非零）
    // → 双射解码与 codec 谓词双拒绝（admission 拒绝可见）。
    std::string upper_variant = ::heyaki::to_string(wire_transfer);
    upper_variant[5] = 'A';  // 大写（非规范）：正确长度的 hyt1_ 大写变体
    std::string padded_variant = ::heyaki::to_string(wire_transfer);
    padded_variant.back() = 'z';  // 末字符低 2 位 = 01（填充位非零）
    const std::vector<std::string> non_canonical{
        std::string{"t-1"},  // ad-hoc 串（M4-04 生成入口定案前的现状形态）
        std::string{"hyt1_short"},  // 长度不符
        std::string{"hym1_00000000000000000000000000000"},  // 错误前缀
        ::heyaki::to_string(wire_transfer) + std::string{"a"},  // 超长
        std::move(upper_variant),
        std::move(padded_variant),
    };
    for (const std::string& bad : non_canonical) {
        CAPTURE(bad);
        REQUIRE_FALSE(aki::conversation::codec::is_canonical_transfer_id_text(
            bad));
        REQUIRE_FALSE(
            aki::heyaki::NodeSession::to_heyaki_transfer_id(
                aki::transfer::TransferId{bad})
                .has_value());
    }

    // aki.image 信封 + ImagePayload 冻结字段号编码：encode → parse 往返
    //（type/schema_version/mode/payload 无损；MessageId 双射同 aki.text）。
    const aki::transfer::FileMetadata media{"photo.png", 2048, "image/png"};
    const auto payload_wire =
        aki::conversation::codec::encode_image_payload({media, aki_transfer});
    REQUIRE(payload_wire.has_value());
    ::heyaki::MessageEnvelope envelope;
    envelope.message_id = fixed_wire_id();
    envelope.type = std::string(
        aki::conversation::codec::kAkiImageEnvelopeType);
    envelope.schema_version =
        aki::conversation::codec::kAkiImagePayloadSchemaVersion;
    envelope.delivery_mode = ::heyaki::MessageDeliveryMode::peer_acked;
    envelope.payload = *payload_wire;
    auto encoded = ::heyaki::encode_message_envelope(envelope);
    REQUIRE(encoded.has_value());
    auto parsed = ::heyaki::parse_message_envelope(
        std::span<const std::byte>(
            encoded.value_if()->data(), encoded.value_if()->size()));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value_if()->message_id == fixed_wire_id());
    REQUIRE(parsed.value_if()->type
        == aki::conversation::codec::kAkiImageEnvelopeType);
    REQUIRE(parsed.value_if()->schema_version
        == aki::conversation::codec::kAkiImagePayloadSchemaVersion);
    REQUIRE(parsed.value_if()->delivery_mode
        == ::heyaki::MessageDeliveryMode::peer_acked);
    const auto decoded = aki::conversation::codec::decode_image_payload(
        std::span<const std::byte>(parsed.value_if()->payload.data(),
            parsed.value_if()->payload.size()));
    REQUIRE(decoded.error
        == aki::conversation::codec::ImagePayloadDecodeError::none);
    REQUIRE(decoded.value.has_value());
    REQUIRE(decoded.value->media == media);
    REQUIRE(decoded.value->transfer_id == aki_transfer);
}

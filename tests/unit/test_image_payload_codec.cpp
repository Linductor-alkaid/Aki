// M4-03：ImagePayload wire 编解码器单测（设计 §6.1①/DEC-010①；网络无关）。
//
// 覆盖：编码-解码往返；各拒收路径逐项（截断/缺字段/超限/非规范 transfer_id/
// 未知字段跳过/预留字段 5 前向容忍/载荷总量上限/重复字段首现确定性）；出站
// 编码对称拒绝。独立 unit 二进制（沿 test_heyaki_message 拆分纪律——本文件
// 无 [skip] 受控退出，始终完整执行）。
#include "conversation/codec/image_payload_codec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace aki::conversation;
using aki::conversation::codec::ImagePayloadDecodeError;
using aki::transfer::FileMetadata;
using aki::transfer::TransferId;

// 规范 transfer_id（与 test_heyaki_message 的 heyaki 编码器交叉验证互补：
// 此处取尾部填充位为零的确定字面值）。
constexpr const char* kCanonicalTransferId = "hyt1_aaaaaaaaaaaaaaaaaaaaaaaaae";

std::vector<std::byte> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
        reinterpret_cast<const std::byte*>(text.data()) + text.size()};
}

// ---- 测试内 wire 构造器（绕过 encode_image_payload，构造非标准输入）----

void put_varint(std::vector<std::byte>& out, std::uint64_t value) {
    while (value >= 0x80U) {
        out.push_back(static_cast<std::byte>(
            static_cast<std::uint8_t>(value) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(value)));
}

void put_header(std::vector<std::byte>& out, std::uint32_t field,
    std::uint32_t wire_type) {
    put_varint(out, (std::uint64_t{field} << 3U) | wire_type);
}

void put_bytes_field(std::vector<std::byte>& out, std::uint32_t field,
    std::string_view value) {
    put_header(out, field, 2U);
    put_varint(out, value.size());
    const auto data = bytes_of(value);
    out.insert(out.end(), data.begin(), data.end());
}

ImagePayload sample() {
    return ImagePayload{
        FileMetadata{"photo.png", 2048, "image/png", ""},
        TransferId{kCanonicalTransferId}};
}

}  // namespace

TEST_CASE("Image payload codec round-trips the frozen v1 fields",
    "[unit][codec][image]") {
    const ImagePayload payload = sample();
    const auto encoded = codec::encode_image_payload(payload);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->size() <= codec::kImagePayloadMaxBytes);

    const auto decoded = codec::decode_image_payload(
        std::span<const std::byte>(encoded->data(), encoded->size()));
    REQUIRE(decoded.error == ImagePayloadDecodeError::none);
    REQUIRE(decoded.value.has_value());
    REQUIRE(decoded.value->media == payload.media);
    REQUIRE(decoded.value->transfer_id == payload.transfer_id);

    // 空串边界：mime_type 可空（≤128B 界内）、name/transfer_id 不可空。
    ImagePayload empty_mime = sample();
    empty_mime.media.mime_type.clear();
    const auto encoded_empty_mime =
        codec::encode_image_payload(empty_mime);
    REQUIRE(encoded_empty_mime.has_value());
    const auto decoded_empty_mime = codec::decode_image_payload(
        std::span<const std::byte>(encoded_empty_mime->data(),
            encoded_empty_mime->size()));
    REQUIRE(decoded_empty_mime.error == ImagePayloadDecodeError::none);
    REQUIRE(decoded_empty_mime.value->media.mime_type.empty());
}

TEST_CASE("Image payload codec rejects bounded violations visibly",
    "[unit][codec][image]") {
    // 出站编码对称拒绝：超长 name/mime、非规范 transfer_id、空 name。
    ImagePayload bad = sample();
    bad.media.name.assign(codec::kImageNameMaxBytes + 1, 'n');
    REQUIRE_FALSE(codec::encode_image_payload(bad).has_value());
    bad = sample();
    bad.media.mime_type.assign(codec::kImageMimeMaxBytes + 1, 'm');
    REQUIRE_FALSE(codec::encode_image_payload(bad).has_value());
    bad = sample();
    bad.transfer_id = TransferId{"t-1"};
    REQUIRE_FALSE(codec::encode_image_payload(bad).has_value());
    bad = sample();
    bad.media.name.clear();
    REQUIRE_FALSE(codec::encode_image_payload(bad).has_value());
    bad = sample();
    bad.transfer_id = TransferId{};
    REQUIRE_FALSE(codec::encode_image_payload(bad).has_value());

    // 入站：截断（tag 后输入耗尽 / varint 中途截断 / 长度越界）。
    const auto encoded = codec::encode_image_payload(sample());
    REQUIRE(encoded.has_value());
    for (std::size_t cut = 0; cut + 1 < encoded->size(); ++cut) {
        const auto decoded = codec::decode_image_payload(
            std::span<const std::byte>(encoded->data(), cut));
        // 截断必然拒收且可见：要么 wire 解析失败、要么必填字段缺失
        //（内容本身未被截断改写——invalid_transfer_id 不在截断可达集）。
        CAPTURE(cut);
        REQUIRE_FALSE(decoded.value.has_value());
        REQUIRE((decoded.error == ImagePayloadDecodeError::malformed_wire
            || decoded.error == ImagePayloadDecodeError::missing_field));
    }

    // 入站：缺必填字段（逐个抽除字段 1~4 的手写 wire）。
    for (const std::uint32_t omitted : {1U, 2U, 3U, 4U}) {
        std::vector<std::byte> wire;
        if (omitted != 1U) put_bytes_field(wire, 1U, "photo.png");
        if (omitted != 2U) {
            put_header(wire, 2U, 0U);
            put_varint(wire, 2048);
        }
        if (omitted != 3U) put_bytes_field(wire, 3U, "image/png");
        if (omitted != 4U) put_bytes_field(wire, 4U, kCanonicalTransferId);
        const auto decoded = codec::decode_image_payload(
            std::span<const std::byte>(wire.data(), wire.size()));
        CAPTURE(omitted);
        REQUIRE(decoded.error == ImagePayloadDecodeError::missing_field);
    }

    // 入站：字段超限（name > 512B / mime > 128B）。
    std::vector<std::byte> oversized;
    put_bytes_field(oversized, 1U, std::string(codec::kImageNameMaxBytes + 1, 'n'));
    put_header(oversized, 2U, 0U);
    put_varint(oversized, 1);
    put_bytes_field(oversized, 3U, "image/png");
    put_bytes_field(oversized, 4U, kCanonicalTransferId);
    REQUIRE(codec::decode_image_payload(
                std::span<const std::byte>(oversized.data(), oversized.size()))
                .error
        == ImagePayloadDecodeError::field_limit);
    std::vector<std::byte> oversized_mime;
    put_bytes_field(oversized_mime, 1U, "photo.png");
    put_header(oversized_mime, 2U, 0U);
    put_varint(oversized_mime, 1);
    put_bytes_field(oversized_mime, 3U,
        std::string(codec::kImageMimeMaxBytes + 1, 'm'));
    put_bytes_field(oversized_mime, 4U, kCanonicalTransferId);
    REQUIRE(codec::decode_image_payload(std::span<const std::byte>(
                oversized_mime.data(), oversized_mime.size()))
                .error
        == ImagePayloadDecodeError::field_limit);

    // 入站：非规范 transfer_id（错误前缀 / 长度 / 大写 / 填充位非零）。
    for (const std::string_view bad_id :
        {"hyt2_aaaaaaaaaaaaaaaaaaaaaaaaae",  // 错误前缀
            "hyt1_short",                    // 长度不符
            "hyt1_Aaaaaaaaaaaaaaaaaaaaaaaae",  // 大写（非规范）
            "hyt1_aaaaaaaaaaaaaaaaaaaaaaaaz"}) {  // 末字符填充位非零
        std::vector<std::byte> wire;
        put_bytes_field(wire, 1U, "photo.png");
        put_header(wire, 2U, 0U);
        put_varint(wire, 2048);
        put_bytes_field(wire, 3U, "image/png");
        put_bytes_field(wire, 4U, bad_id);
        const auto decoded = codec::decode_image_payload(
            std::span<const std::byte>(wire.data(), wire.size()));
        CAPTURE(bad_id);
        REQUIRE(decoded.error
            == ImagePayloadDecodeError::invalid_transfer_id);
    }

    // 入站：载荷总量超上限（4KiB，先于解析拒绝）。
    std::vector<std::byte> huge(codec::kImagePayloadMaxBytes + 1, std::byte{0});
    REQUIRE(codec::decode_image_payload(
                std::span<const std::byte>(huge.data(), huge.size()))
                .error
        == ImagePayloadDecodeError::payload_too_large);

    // 入站：非法 wire（field_number 0 / 保留 wire 类型 5）。
    std::vector<std::byte> zero_field;
    put_header(zero_field, 0U, 2U);
    put_varint(zero_field, 0);
    REQUIRE(codec::decode_image_payload(std::span<const std::byte>(
                zero_field.data(), zero_field.size()))
                .error
        == ImagePayloadDecodeError::malformed_wire);
    std::vector<std::byte> fixed32;
    put_header(fixed32, 9U, 5U);
    fixed32.insert(fixed32.end(), 4, std::byte{0});
    REQUIRE(codec::decode_image_payload(std::span<const std::byte>(
                fixed32.data(), fixed32.size()))
                .error
        == ImagePayloadDecodeError::malformed_wire);
}

TEST_CASE("Image payload codec skips unknown fields for forward tolerance",
    "[unit][codec][image]") {
    // 未知字段号（99/100）跳过（前向容忍，DEC-010① 版本规则）；字段 5 自
    // M4-04 起承载 stored_sha256（可选取值——缺省视为无；首现为准）。
    const auto encoded = codec::encode_image_payload(sample());
    REQUIRE(encoded.has_value());
    std::vector<std::byte> extended = *encoded;
    const std::string sha_field(64, 'h');
    put_bytes_field(extended, 5U, sha_field);  // stored_sha256（M4-04 起在册）
    put_bytes_field(extended, 99U, "future-field");
    {
        put_header(extended, 100U, 0U);  // 未知 varint 字段
        put_varint(extended, 12345);
    }
    const auto decoded = codec::decode_image_payload(
        std::span<const std::byte>(extended.data(), extended.size()));
    REQUIRE(decoded.error == ImagePayloadDecodeError::none);
    REQUIRE(decoded.value.has_value());
    REQUIRE(decoded.value->media.name == sample().media.name);
    REQUIRE(decoded.value->media.size_bytes == sample().media.size_bytes);
    REQUIRE(decoded.value->media.mime_type == sample().media.mime_type);
    REQUIRE(decoded.value->media.stored_sha256 == sha_field);
    REQUIRE(decoded.value->transfer_id == sample().transfer_id);

    // 重复字段：首现为准（确定性——不做 last-wins，避免歧义语义入契约）。
    std::vector<std::byte> duplicated;
    put_bytes_field(duplicated, 1U, "first.png");
    put_bytes_field(duplicated, 1U, "second.png");
    put_header(duplicated, 2U, 0U);
    put_varint(duplicated, 1);
    put_bytes_field(duplicated, 3U, "image/png");
    put_bytes_field(duplicated, 4U, kCanonicalTransferId);
    put_bytes_field(duplicated, 5U, sha_field);
    put_bytes_field(duplicated, 5U, std::string(64, 'x'));
    const auto decoded_duplicate = codec::decode_image_payload(
        std::span<const std::byte>(duplicated.data(), duplicated.size()));
    REQUIRE(decoded_duplicate.error == ImagePayloadDecodeError::none);
    REQUIRE(decoded_duplicate.value->media.name == "first.png");
    REQUIRE(decoded_duplicate.value->media.stored_sha256 == sha_field);
}

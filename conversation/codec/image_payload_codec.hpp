// ImagePayload wire 编解码器（设计 §6.1①/DEC-010；DEC-006 冻结常量）。
//
// aki.image 信封载荷的 aki 自有 protobuf-wire 格式最小编解码器（冻结字段号
// schema v1，落 §14 预留的 conversation/codec/）：
//   1 = name        length-delimited，≤512B（对齐 heyaki max_logical_name_bytes）
//   2 = size_bytes  varint
//   3 = mime_type   length-delimited，≤128B
//   4 = transfer_id length-delimited，规范串 hyt1_ + 26 base32 = 31 字符
//   5 = stored_sha256（预留 M4-04；v1 解码器跳过，不读取）
//
// 解析规则（前向容忍 + 有界拒绝可见，RULE-09）：
//   - 跳过未知字段号（追加可选字段不 bump schema_version，破坏性变更才 bump）；
//   - 缺必填 1~4、字段超限、transfer_id 非规范（前缀/长度/字符集/尾部位填充
//     任一不符）或总量超 aki 侧上限 4KiB → 解码拒绝（error 可见，调用方不
//     投递下游）；出站对称：超限/非规范 → 编码 nullopt（SPI admission false）。
//   - 字符集仅长度界，不校验 UTF-8（沿 aki.text 姿态，防止实现各自加码）。
//
// 本头文件属 Domain 层（RULE-01/10）：不依赖 heyaki/third_party——transfer_id
// 规范形式的完整谓词（base32 双射 + 尾部填充位为零）在此以纯字符串逻辑冻结；
// heyaki::parse_transfer_id 的权威校验在 Adapter 层出站路径另行执行（DEC-010）。
#pragma once

#include "conversation/message/message_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aki::conversation::codec {

// ---- 冻结常量（DEC-006 冻结常量扩展 / DEC-010 ①）----

inline constexpr std::string_view kAkiImageEnvelopeType = "aki.image";
inline constexpr std::uint32_t kAkiImagePayloadSchemaVersion = 1;

inline constexpr std::size_t kImageNameMaxBytes = 512;
inline constexpr std::size_t kImageMimeMaxBytes = 128;
// hyt1_ 前缀（5）+ 16 字节 base32 编码（ceil(128/5) = 26）= 31 字符。
inline constexpr std::size_t kImageTransferIdEncodedBytes = 31;
// aki 侧载荷总量上限（紧于 heyaki envelope 1MiB；冻结于 codec 常量）。
inline constexpr std::size_t kImagePayloadMaxBytes = 4096;

enum class ImagePayloadDecodeError {
    none,
    payload_too_large,   // 总量超 kImagePayloadMaxBytes（先于解析拒绝）
    malformed_wire,      // 截断 / 非法 tag-wire 组合 / 保留 wire 类型
    field_limit,         // 已知字段超限（name/mime）
    missing_field,       // 缺必填字段 1~4
    invalid_transfer_id, // 字段 4 非规范 hyt1_ 形式
};

struct ImagePayloadDecodeResult {
    std::optional<ImagePayload> value;
    ImagePayloadDecodeError error = ImagePayloadDecodeError::none;
};

// transfer_id 规范形式完整谓词（与 heyaki parse_transfer_id 的接受集一致）：
// hyt1_ 前缀 + 26 个小写 base32 字符（a-z、2-7）+ 尾部填充位为零
//（128 位 = 25.6 字符 → 末字符低 3 位有效、高 2 位须为零）。
[[nodiscard]] inline bool is_canonical_transfer_id_text(std::string_view text) {
    constexpr std::string_view kPrefix = "hyt1_";
    if (text.size() != kImageTransferIdEncodedBytes
        || !text.starts_with(kPrefix)) {
        return false;
    }
    // 128 数据位 / 5 = 25 个完整字符 + 1 个字符承载余 3 位。
    std::uint32_t accumulator = 0;
    unsigned int bits = 0;
    for (const char character : text.substr(kPrefix.size())) {
        int value = -1;
        if (character >= 'a' && character <= 'z') {
            value = character - 'a';
        } else if (character >= '2' && character <= '7') {
            value = character - '2' + 26;
        }
        if (value < 0) {
            return false;  // 含大写/非法字符（大写亦非规范）
        }
        accumulator = (accumulator << 5U) | static_cast<std::uint32_t>(value);
        bits += 5U;
        if (bits >= 8U) {
            bits -= 8U;
            accumulator &= (1U << bits) - 1U;
        }
    }
    return bits == 0U || accumulator == 0U;  // 填充位须为零
}

// ---- protobuf-wire 最小原语（field_number << 3 | wire_type）----

namespace wire_detail {

inline constexpr std::uint32_t kWireVarint = 0;
inline constexpr std::uint32_t kWireLengthDelimited = 2;

[[nodiscard]] inline bool append_varint(std::vector<std::byte>& out,
    std::uint64_t value) {
    if (out.size() >= kImagePayloadMaxBytes) {
        return false;  // 总量上限（先于写入拒绝）
    }
    while (value >= 0x80U) {
        out.push_back(static_cast<std::byte>(
            static_cast<std::uint8_t>(value) | 0x80U));
        value >>= 7U;
        if (out.size() >= kImagePayloadMaxBytes) {
            return false;
        }
    }
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(value)));
    return true;
}

[[nodiscard]] inline bool append_header(std::vector<std::byte>& out,
    std::uint32_t field_number, std::uint32_t wire_type) {
    return append_varint(out,
        (static_cast<std::uint64_t>(field_number) << 3U) | wire_type);
}

[[nodiscard]] inline bool append_bytes_field(std::vector<std::byte>& out,
    std::uint32_t field_number, std::string_view bytes) {
    if (!append_header(out, field_number, kWireLengthDelimited)
        || !append_varint(out, bytes.size())) {
        return false;
    }
    out.insert(out.end(),
        reinterpret_cast<const std::byte*>(bytes.data()),
        reinterpret_cast<const std::byte*>(bytes.data()) + bytes.size());
    return true;
}

// varint 解码游标。truncated_out = true 表示输入耗尽（截断）。
[[nodiscard]] inline bool read_varint(std::span<const std::byte> bytes,
    std::size_t& cursor, std::uint64_t& value_out) {
    value_out = 0;
    unsigned int shift = 0;
    for (;;) {
        if (cursor >= bytes.size() || shift >= 64U) {
            return false;  // 截断或超长 varint
        }
        const auto byte = std::to_integer<std::uint8_t>(bytes[cursor++]);
        value_out |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0U) {
            return true;
        }
        shift += 7U;
    }
}

}  // namespace wire_detail

// ---- 出站编码：超限/非规范 transfer_id/空 name → nullopt（admission false）----

[[nodiscard]] inline std::optional<std::vector<std::byte>> encode_image_payload(
    const ImagePayload& payload) {
    if (payload.media.name.empty()
        || payload.media.name.size() > kImageNameMaxBytes
        || payload.media.mime_type.size() > kImageMimeMaxBytes
        || !is_canonical_transfer_id_text(payload.transfer_id.value)) {
        return std::nullopt;  // 有界拒绝可见（RULE-09）
    }
    std::vector<std::byte> out;
    out.reserve(kImageTransferIdEncodedBytes + payload.media.name.size()
        + payload.media.mime_type.size() + 16);
    if (!wire_detail::append_bytes_field(out, 1, payload.media.name)
        || !wire_detail::append_header(out, 2, wire_detail::kWireVarint)
        || !wire_detail::append_varint(
            out, static_cast<std::uint64_t>(payload.media.size_bytes))
        || !wire_detail::append_bytes_field(out, 3, payload.media.mime_type)
        || !wire_detail::append_bytes_field(
            out, 4, payload.transfer_id.value)
        || out.size() > kImagePayloadMaxBytes) {
        return std::nullopt;  // 编码超限：总量上限在成品上校验（先写入有界）
    }
    return out;
}

// ---- 入站解码：跳过未知字段；缺必填/超限/非规范 → error 可见 ----
//
// ImagePayloadDecodeResult.error 携带拒收原因（调用方计数/日志可观测）；
// value 仅在 error == none 时有值。
[[nodiscard]] inline ImagePayloadDecodeResult decode_image_payload(
    std::span<const std::byte> bytes) {
    if (bytes.size() > kImagePayloadMaxBytes) {
        return {std::nullopt, ImagePayloadDecodeError::payload_too_large};
    }
    std::optional<std::string> name;
    std::optional<std::uint64_t> size_bytes;
    std::optional<std::string> mime_type;
    std::optional<std::string> transfer_id;
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        std::uint64_t tag = 0;
        if (!wire_detail::read_varint(bytes, cursor, tag)) {
            return {std::nullopt, ImagePayloadDecodeError::malformed_wire};
        }
        const auto field_number = static_cast<std::uint32_t>(tag >> 3U);
        const auto wire_type = static_cast<std::uint32_t>(tag & 0x7U);
        if (field_number == 0U) {
            return {std::nullopt, ImagePayloadDecodeError::malformed_wire};
        }
        if (wire_type == wire_detail::kWireVarint) {
            std::uint64_t value = 0;
            if (!wire_detail::read_varint(bytes, cursor, value)) {
                return {std::nullopt,
                    ImagePayloadDecodeError::malformed_wire};
            }
            if (field_number == 2U && !size_bytes.has_value()) {
                size_bytes = value;
            }
            // 其余 varint 字段：未知/重复均可跳过（重复以首现为准，保持
            // 确定性；必填缺省仍由末尾校验兜住）。
        } else if (wire_type == wire_detail::kWireLengthDelimited) {
            std::uint64_t length = 0;
            if (!wire_detail::read_varint(bytes, cursor, length)
                || length > bytes.size() - cursor) {
                return {std::nullopt,
                    ImagePayloadDecodeError::malformed_wire};
            }
            const auto* begin =
                reinterpret_cast<const char*>(bytes.data() + cursor);
            const std::string_view field_text(begin,
                static_cast<std::size_t>(length));
            cursor += static_cast<std::size_t>(length);
            switch (field_number) {
                case 1U:
                    if (field_text.size() > kImageNameMaxBytes) {
                        return {std::nullopt,
                            ImagePayloadDecodeError::field_limit};
                    }
                    if (!name.has_value()) {
                        name = std::string{field_text};
                    }
                    break;
                case 3U:
                    if (field_text.size() > kImageMimeMaxBytes) {
                        return {std::nullopt,
                            ImagePayloadDecodeError::field_limit};
                    }
                    if (!mime_type.has_value()) {
                        mime_type = std::string{field_text};
                    }
                    break;
                case 4U:
                    if (!is_canonical_transfer_id_text(field_text)) {
                        return {std::nullopt,
                            ImagePayloadDecodeError::invalid_transfer_id};
                    }
                    if (!transfer_id.has_value()) {
                        transfer_id = std::string{field_text};
                    }
                    break;
                default:
                    break;  // 未知字段（含预留 5）：跳过（前向容忍）
            }
        } else {
            // wire 类型 1/5（64/32-bit）与已弃用 group（3/4）：aki schema v1
            // 未使用，按 malformed 拒绝——不支持半解析跳过，保持最小面。
            return {std::nullopt, ImagePayloadDecodeError::malformed_wire};
        }
    }
    if (!name.has_value() || !size_bytes.has_value()
        || !mime_type.has_value() || !transfer_id.has_value()) {
        return {std::nullopt, ImagePayloadDecodeError::missing_field};
    }
    ImagePayload payload;
    payload.media = FileMetadata{*name, *size_bytes, *mime_type};
    payload.transfer_id = TransferId{*transfer_id};
    return {std::move(payload), ImagePayloadDecodeError::none};
}

}  // namespace aki::conversation::codec

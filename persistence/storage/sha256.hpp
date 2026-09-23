// 流式 SHA-256（FIPS 180-4；DEC-004：Completed 终态流式计算文件哈希；M2-06）。
// 自包含实现，无平台/第三方依赖（RULE-10）。公开面仅 std 类型。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace aki::persistence {

class Sha256 {
public:
    Sha256() noexcept;

    void update(std::span<const std::byte> data) noexcept;
    // 结束流并返回 64 字符小写十六进制摘要；此后对象不可继续 update。
    [[nodiscard]] std::string final_hex();

private:
    void compress(const std::uint8_t (&block)[64]) noexcept;

    std::uint32_t state_[8];
    std::uint8_t buffer_[64];
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
    bool finalized_ = false;
};

// 一次性摘要便捷接口。
[[nodiscard]] std::string sha256_hex(std::span<const std::byte> data);

}  // namespace aki::persistence

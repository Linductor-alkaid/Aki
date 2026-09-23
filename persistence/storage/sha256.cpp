// 流式 SHA-256 实现（FIPS 180-4；M2-06）。
#include "persistence/storage/sha256.hpp"

#include <array>
#include <utility>

namespace aki::persistence {
namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t x, int n) noexcept {
    return (x >> n) | (x << (32 - n));
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f,
          0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::compress(const std::uint8_t (&block)[64]) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24)
            | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
            | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
            | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
    if (finalized_) {
        return;
    }
    std::size_t size = data.size();
    total_bytes_ += size;

    std::size_t offset = 0;
    if (buffer_size_ > 0) {
        const std::size_t take = (64 - buffer_size_) < size
            ? (64 - buffer_size_)
            : size;
        for (std::size_t i = 0; i < take; ++i) {
            buffer_[buffer_size_ + i] =
                static_cast<std::uint8_t>(data[offset + i]);
        }
        buffer_size_ += take;
        offset += take;
        size -= take;
        if (buffer_size_ == 64) {
            compress(buffer_);
            buffer_size_ = 0;
        }
    }
    while (size >= 64) {
        std::uint8_t block[64];
        for (std::size_t i = 0; i < 64; ++i) {
            block[i] = static_cast<std::uint8_t>(data[offset + i]);
        }
        compress(block);
        offset += 64;
        size -= 64;
    }
    if (size > 0) {
        // 仅在有尾部时改写缓冲：本次数据恰好填满先前部分缓冲（size == 0）
        // 时必须保留 buffer_size_——无条件 buffer_size_ = size 会把已缓冲
        // 字节清零丢弃，流式摘要错误。（不变式：到此处且 size > 0 时
        // buffer_size_ 必为 0；写法不依赖该不变式，防后续重排静默劣化。）
        for (std::size_t i = 0; i < size; ++i) {
            buffer_[buffer_size_ + i] =
                static_cast<std::uint8_t>(data[offset + i]);
        }
        buffer_size_ += size;
    }
}

std::string Sha256::final_hex() {
    if (!finalized_) {
        const std::uint64_t bits = total_bytes_ * 8;
        buffer_[buffer_size_++] = 0x80;
        while (buffer_size_ != 56) {
            if (buffer_size_ < 64) {
                buffer_[buffer_size_++] = 0x00;
            } else {
                compress(buffer_);
                buffer_size_ = 0;
            }
        }
        for (int i = 0; i < 8; ++i) {
            buffer_[56 + i] =
                static_cast<std::uint8_t>((bits >> (56 - i * 8)) & 0xFF);
        }
        compress(buffer_);
        finalized_ = true;
    }
    static const char* kHex = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const std::uint32_t word : state_) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            hex += kHex[(word >> shift) & 0xF];
        }
    }
    return hex;
}

std::string sha256_hex(std::span<const std::byte> data) {
    Sha256 hash;
    hash.update(data);
    return hash.final_hex();
}

}  // namespace aki::persistence

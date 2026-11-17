#include "net/digest.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace net {

namespace {

constexpr uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

// The message padded to whole 64-byte blocks: a 1 bit, zeros, its length in
// bits (big-endian for SHA-1, little-endian for MD5).
std::vector<uint8_t> padded(std::span<const uint8_t> data, bool big_endian) {
    std::vector<uint8_t> m(data.begin(), data.end());
    const uint64_t bits = uint64_t{data.size()} * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 0; i < 8; ++i) m.push_back(static_cast<uint8_t>(bits >> (big_endian ? 56 - 8 * i : 8 * i)));
    return m;
}

} // namespace

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const std::vector<uint8_t> m = padded(data, true);
    for (std::size_t block = 0; block < m.size(); block += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = &m[block + 4 * static_cast<std::size_t>(i)];
            w[i] = uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::array<uint8_t, 20> out;
    for (int i = 0; i < 20; ++i) out[static_cast<std::size_t>(i)] = static_cast<uint8_t>(h[i / 4] >> (24 - 8 * (i % 4)));
    return out;
}

std::array<uint8_t, 20> hmac_sha1(std::span<const uint8_t> key, std::span<const uint8_t> data) {
    uint8_t k[64] = {};
    if (key.size() > 64) {
        const auto hashed = sha1(key);
        std::memcpy(k, hashed.data(), hashed.size());
    } else if (!key.empty()) {
        std::memcpy(k, key.data(), key.size());
    }
    std::vector<uint8_t> inner(64), outer(64);
    for (int i = 0; i < 64; ++i) inner[static_cast<std::size_t>(i)] = k[i] ^ 0x36, outer[static_cast<std::size_t>(i)] = k[i] ^ 0x5C;
    inner.insert(inner.end(), data.begin(), data.end());
    const auto first = sha1(inner);
    outer.insert(outer.end(), first.begin(), first.end());
    return sha1(outer);
}

std::array<uint8_t, 16> md5(std::span<const uint8_t> data) {
    static constexpr uint32_t kK[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr int kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                   14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const std::vector<uint8_t> m = padded(data, false);
    for (std::size_t block = 0; block < m.size(); block += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = &m[block + 4 * static_cast<std::size_t>(i)];
            w[i] = uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) f = (b & c) | (~b & d), g = i;
            else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else f = c ^ (b | ~d), g = (7 * i) % 16;
            const uint32_t t = d;
            d = c, c = b;
            b = b + rol(a + f + kK[i] + w[g], kS[i]);
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d;
    }
    std::array<uint8_t, 16> out;
    for (int i = 0; i < 16; ++i) out[static_cast<std::size_t>(i)] = static_cast<uint8_t>(h[i / 4] >> (8 * (i % 4)));
    return out;
}

// SHA-256 (FIPS 180-4, section 6.2)
namespace {
constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
} // namespace

Sha256::Sha256() : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, buf_{} {}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t{p[4 * i]} << 24 | uint32_t{p[4 * i + 1]} << 16 | uint32_t{p[4 * i + 2]} << 8 | uint32_t{p[4 * i + 3]};
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
        const uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::add(const uint8_t* data, std::size_t size) {
    total_ += size;
    while (size > 0) {
        if (used_ == 0 && size >= 64) {
            block(data), data += 64, size -= 64;
            continue;
        }
        const std::size_t n = std::min<std::size_t>(64 - used_, size);
        std::memcpy(buf_ + used_, data, n);
        used_ += n, data += n, size -= n;
        if (used_ == 64) block(buf_), used_ = 0;
    }
}

std::array<uint8_t, 32> Sha256::finish() {
    const uint64_t bits = total_ * 8;
    const uint8_t one = 0x80, zero = 0;
    add(&one, 1);
    while (used_ != 56) add(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    add(len, 8);
    std::array<uint8_t, 32> out;
    for (int i = 0; i < 32; ++i) out[static_cast<std::size_t>(i)] = static_cast<uint8_t>(h_[i / 4] >> (24 - 8 * (i % 4)));
    return out;
}

std::array<uint8_t, 32> sha256(std::span<const uint8_t> data) {
    Sha256 s;
    s.add(data.data(), data.size());
    return s.finish();
}

} // namespace net

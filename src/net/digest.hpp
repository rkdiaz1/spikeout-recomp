// SHA-1, HMAC-SHA1 and MD5, as STUN and TURN's long-term credentials need
// them (RFC 5389: MESSAGE-INTEGRITY is HMAC-SHA1 keyed with
// MD5(username:realm:password)). Written from RFC 3174, 2104 and 1321. And SHA-256 (FIPS 180-4).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace net {

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data);
std::array<uint8_t, 20> hmac_sha1(std::span<const uint8_t> key, std::span<const uint8_t> data);
std::array<uint8_t, 16> md5(std::span<const uint8_t> data);

// SHA-256 (FIPS 180-4), fed in pieces: GitHub's digest of a release's files (the in-game update, net/install.hpp).
class Sha256 {
public:
    Sha256();
    void add(const uint8_t* data, std::size_t size);
    std::array<uint8_t, 32> finish();

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    std::size_t used_ = 0;
    uint64_t total_ = 0;
};
std::array<uint8_t, 32> sha256(std::span<const uint8_t> data);

inline std::span<const uint8_t> bytes_of(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

} // namespace net

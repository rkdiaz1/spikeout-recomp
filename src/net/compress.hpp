// Save states sent over the network, compressed with zstd: a board's 29 MB
// state is 3-5 MB so (docs/netplay.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace net {

std::vector<uint8_t> compress(std::span<const uint8_t> data, int level = 3);
// Empty if `data` isn't zstd's (or is damaged), or would unpack to more than `max_bytes` (safe loading, release 6:
// what another player sends can't make this one's game take gigabytes; every board's state of a room of four is
// ~112 MB, a board's ~29 MB).
constexpr std::size_t kMaxRoomState = 160u << 20, kMaxBoardState = 48u << 20, kMaxPacket = 4u << 20;
std::optional<std::vector<uint8_t>> decompress(std::span<const uint8_t> data, std::size_t max_bytes = kMaxRoomState);
// The same against a reference the unpacking side has too (zstd's prefix): a chunk of one board's state against the
// first board's at the same place, the boards much alike (a joiner's state in chunks from several players, Session's
// share: 7.9 MB for a room of four, 22 MB with each chunk alone). Empty `ref`: as compress / decompress.
std::vector<uint8_t> compress_against(std::span<const uint8_t> data, std::span<const uint8_t> ref, int level = 3);
std::optional<std::vector<uint8_t>> decompress_against(std::span<const uint8_t> data, std::span<const uint8_t> ref, std::size_t max_bytes);

} // namespace net

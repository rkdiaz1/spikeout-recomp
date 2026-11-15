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

} // namespace net

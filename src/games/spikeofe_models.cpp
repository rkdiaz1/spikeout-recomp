#include "games/spikeofe_models.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace m3::games {

namespace {

// Where the game keeps them.
constexpr uint32_t kNames = 0xFFF78104;      // program ROM: a count, then a pointer to each name (0-terminated)
constexpr uint32_t kNameRanges = 0xFFA00000; // program ROM: (first model number, count) pairs, in the names' order
constexpr uint32_t kModelTable = 0x7204B0;   // RAM: a word a model number, the culling node it starts at
constexpr uint32_t kModels = 9110;           // model numbers 0 .. 9109
constexpr uint32_t kMaxNames = 20000;
constexpr std::size_t kMaxName = 64;

uint8_t rom_byte(const RomSet& roms, uint32_t addr) {
    return static_cast<uint8_t>(roms.read_program32(addr & ~3u) >> (8 * (3 - (addr & 3))));
}

} // namespace

void SpikeoutModels::load(const RomSet& roms) {
    animation_.assign(kModels, 0);
    named_ = animations_ = 0;
    const uint32_t count = roms.read_program32(kNames);
    if (count == 0 || count > kMaxNames) return;
    // Each name's model number, from the ranges.
    std::vector<uint32_t> numbers;
    for (uint32_t r = 0; numbers.size() < count && r < 64; ++r) {
        const uint32_t first = roms.read_program32(kNameRanges + 8 * r), n = roms.read_program32(kNameRanges + 8 * r + 4);
        if (n == 0 || first + n > kModels) return; // not the ROM this was read from
        for (uint32_t k = 0; k < n && numbers.size() < count; ++k) numbers.push_back(first + k);
    }
    if (numbers.size() != count) return;
    // The stems: a name without its trailing number. Detail levels (a number
    // of 300, 500 or 700 after an underscore) are one model drawn at several
    // sizes, not an animation.
    std::unordered_map<std::string, std::vector<uint32_t>> stems;
    for (uint32_t k = 0; k < count; ++k) {
        const uint32_t at = roms.read_program32(kNames + 4 + 4 * k);
        if (at < 0xFF800000u) continue;
        std::string name;
        for (uint32_t i = 0; i < kMaxName; ++i) {
            const uint8_t c = rom_byte(roms, at + i);
            if (c == 0) break;
            name.push_back(static_cast<char>(c));
        }
        if (name.empty()) continue;
        ++named_;
        std::size_t digits = name.size();
        while (digits > 0 && name[digits - 1] >= '0' && name[digits - 1] <= '9') --digits;
        if (digits == name.size() || digits == 0) continue;
        const std::string number = name.substr(digits);
        if (name[digits - 1] == '_' && (number == "300" || number == "500" || number == "700")) continue;
        stems[name.substr(0, digits)].push_back(numbers[k]);
    }
    for (const auto& [stem, members] : stems) {
        if (members.size() < 2) continue;
        ++animations_;
        for (uint32_t n : members) animation_[n] = static_cast<uint32_t>(animations_);
    }
}

void SpikeoutModels::refresh(std::span<const uint8_t> ram) {
    if (kModelTable + 4 * kModels > ram.size()) return;
    const uint8_t* table = ram.data() + kModelTable;
    if (table_.size() == 4 * kModels && std::memcmp(table_.data(), table, table_.size()) == 0) return;
    table_.assign(table, table + 4 * kModels);
    nodes_.clear();
    for (uint32_t n = 0; n < kModels; ++n) {
        const uint8_t* w = table + 4 * n;
        const uint32_t node = (uint32_t{w[0]} << 24 | uint32_t{w[1]} << 16 | uint32_t{w[2]} << 8 | w[3]) & 0xFFFFFF;
        if (node) nodes_.emplace(node, n);
    }
}

void SpikeoutModels::tag(Real3dScene& scene) const {
    for (Real3dScene::Model& model : scene.models)
        model.animation = model.number < animation_.size() ? animation_[model.number] : 0;
}

} // namespace m3::games

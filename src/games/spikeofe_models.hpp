// Smooth motion: which of Spikeout's models belong to one animation (a
// flipbook: the sea's surface, a gull's wing beats, an effect's frames). The
// game names every model (in its program ROM) and keeps, in RAM, the culling
// node each model starts at; the models of one animation share a name but for
// its trailing number. Everything here is read at run time from the player's
// own ROM and RAM and kept only in memory (docs/smooth-motion.md, layer 3).
#pragma once

#include "runtime/real3d_scene.hpp"
#include "runtime/rom.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace m3::games {

class SpikeoutModels {
public:
    // The names, once (another ROM's or none: no animations).
    void load(const RomSet& roms);
    // Before each build: the RAM's table of where each model starts (it
    // changes only when a stage loads), for Real3dSceneBuilder::set_model_nodes.
    void refresh(std::span<const uint8_t> ram);
    const std::unordered_map<uint32_t, uint32_t>* nodes() const { return &nodes_; }
    // After a build: each model's animation (Real3dScene::Model::animation).
    void tag(Real3dScene& scene) const;

    std::size_t models_named() const { return named_; }
    std::size_t animations() const { return animations_; }
    // Tests: the model numbers whose names start with the text in the
    // environment variable SPIKEOUT_MODELS_NAMED (nothing kept otherwise).
    const std::vector<uint32_t>& named_like() const { return named_like_; }

private:
    std::vector<uint32_t> named_like_;
    std::vector<uint32_t> animation_; // by model number: 0 none, else the animation's number
    std::vector<uint8_t> table_;      // the RAM table as last read
    std::unordered_map<uint32_t, uint32_t> nodes_;
    std::size_t named_ = 0, animations_ = 0;
};

} // namespace m3::games

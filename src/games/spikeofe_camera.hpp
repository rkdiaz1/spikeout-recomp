// Smooth motion: Spikeout's camera, read from its RAM after a frame (never
// written). The game has one camera for all its 3D; its view matrix goes into
// the Real3D's matrix table, and each 3D viewport's first culling node points
// at it (m3::Real3dScene::Viewport::camera_words holds the same floats). What
// smooth motion needs to blend it and to tell a cut (docs/smooth-motion.md).
#pragma once

#include "runtime/real3d_scene.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace m3::games {

// The camera's work area in RAM and its fields (offsets from it).
constexpr uint32_t kCameraWork = 0x11D480;
constexpr uint32_t kCameraView = 1600;     // its view matrix: 12 floats, the rotation (column-major), then the translation
constexpr uint32_t kCameraEye = 276;       // the eye before the shake (3 floats)
constexpr uint32_t kCameraLook = 288;      // the point it looks at, before the shake (3 floats)
constexpr uint32_t kCameraEyeShaken = 16;  // the eye the view is made from (3 floats): the shake added to its height
constexpr uint32_t kCameraDriver = 1024;   // the byte saying what moves it (follow, fixed, scripted ...)
constexpr uint32_t kCameraShotTimer = 348; // a win pose's shots (a halfword: 640 while one runs)
constexpr uint32_t kCameraShot = 275;      // the shot's index (a byte)

// `ram`: the board's main RAM. Fills `out` (known = true).
void spikeout_camera(std::span<const uint8_t> ram, Real3dScene::GameCamera& out);

// The bodies (players and enemies): their records in RAM.
constexpr uint32_t kPlayers = 0x1A0000, kEnemies = 0x1B0000, kBodyStride = 0x4000;
constexpr uint32_t kPlayerSlots = 4, kEnemySlots = 16;
constexpr uint32_t kBodyPlace = 896;      // its place in the world (3 floats)
constexpr uint32_t kBodyBones = 860;      // its bones' matrices in the world, 48 bytes each: the root, then the 19 drawn
constexpr uint32_t kBodyRespawn = 16162;  // a byte set to kRespawned when it's put back on the ground, then counting down
constexpr uint8_t kRespawned = 216;
constexpr float kBodyJump = 1.5f;         // world units in a frame: more is a placement (fastest move seen 0.95)

// The effects: a list of 40 records, 24 bytes each, at the address in kEffectList; each kind's line (20 bytes) in
// kEffectKinds (the decompile's effect_draw_all / effect_draw_one; docs/smooth-motion.md).
constexpr uint32_t kEffectList = 0x104058; // a word: where the list is
constexpr uint32_t kEffects = 40, kEffectStride = 24;
constexpr uint32_t kEffectKind = 12, kEffectLeft = 13, kEffectHeading = 16; // (+0: its place, 3 floats)
constexpr uint32_t kEffectKinds = 0x93424, kEffectKindStride = 20, kEffectKindPull = 8;

// Reads the camera after each frame and keeps the one before: a scene's
// game_camera and game_camera_before; and the bodies put down in one jump
// (Real3dScene::placed: those of this read and the one before, as the 3D
// draws the bodies a frame late). Read again with nothing changed (a paused
// frame made again), the one before stays.
class SpikeoutCamera {
public:
    void read(std::span<const uint8_t> ram, Real3dScene& scene);

private:
    struct Body {
        uint32_t word0 = 0;
        std::array<float, 3> place{};
        uint8_t respawn = 0;
        bool operator==(const Body&) const = default;
    };
    Real3dScene::GameCamera last_, before_;
    std::array<Body, kPlayerSlots + kEnemySlots> bodies_{};
    bool bodies_read_ = false;
    std::vector<std::array<float, 3>> placed_now_, placed_before_;
    std::vector<Real3dScene::GameBody> skeletons_, skeletons_before_;
    std::vector<Real3dScene::GameEffect> effects_, effects_before_;
};

} // namespace m3::games

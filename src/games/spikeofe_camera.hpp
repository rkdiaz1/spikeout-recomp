// Smooth motion: Spikeout's camera, read from its RAM after a frame (never
// written). The game has one camera for all its 3D; its view matrix goes into
// the Real3D's matrix table, and each 3D viewport's first culling node points
// at it (m3::Real3dScene::Viewport::camera_words holds the same floats). What
// smooth motion needs to blend it and to tell a cut (docs/smooth-motion.md).
#pragma once

#include "runtime/real3d_scene.hpp"

#include <cstdint>
#include <span>

namespace m3::games {

// The camera's work area in RAM and its fields (offsets from it).
constexpr uint32_t kCameraWork = 0x11D480;
constexpr uint32_t kCameraView = 1600;    // its view matrix: 12 floats, the rotation (column-major), then the translation
constexpr uint32_t kCameraLook = 288;     // the point it looks at (3 floats), as the camera's update left it
constexpr uint32_t kCameraDriver = 1024;  // the byte saying what moves it (follow, fixed, scripted ...)
constexpr uint32_t kCameraShakeLeft = 211; // frames of camera shake left (a byte)

// `ram`: the board's main RAM. Fills `out` (known = true).
void spikeout_camera(std::span<const uint8_t> ram, Real3dScene::GameCamera& out);

} // namespace m3::games

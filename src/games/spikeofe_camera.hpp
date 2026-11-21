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
constexpr uint32_t kCameraView = 1600;     // its view matrix: 12 floats, the rotation (column-major), then the translation
constexpr uint32_t kCameraEye = 276;       // the eye before the shake (3 floats)
constexpr uint32_t kCameraLook = 288;      // the point it looks at, before the shake (3 floats)
constexpr uint32_t kCameraEyeShaken = 16;  // the eye the view is made from (3 floats): the shake added to its height
constexpr uint32_t kCameraDriver = 1024;   // the byte saying what moves it (follow, fixed, scripted ...)
constexpr uint32_t kCameraShotTimer = 348; // a win pose's shots (a halfword: 640 while one runs)
constexpr uint32_t kCameraShot = 275;      // the shot's index (a byte)

// `ram`: the board's main RAM. Fills `out` (known = true).
void spikeout_camera(std::span<const uint8_t> ram, Real3dScene::GameCamera& out);

// Reads the camera after each frame and keeps the one before: a scene's
// game_camera and game_camera_before. Read again with nothing changed (a
// paused frame made again), the one before stays.
class SpikeoutCamera {
public:
    void read(std::span<const uint8_t> ram, Real3dScene& scene) {
        Real3dScene::GameCamera now;
        spikeout_camera(ram, now);
        if (now != last_) {
            before_ = last_;
            last_ = now;
        }
        scene.game_camera = last_;
        scene.game_camera_before = before_;
    }

private:
    Real3dScene::GameCamera last_, before_;
};

} // namespace m3::games

// Smooth motion: the 3D between two frames' scenes, after vf3-recomp's
// (src/video/r3d_scene.h, interpolate; MIT, Copyright (c) 2026 mr-finster),
// for pictures between the arcade's 57.5 frames a second.
//
// Each of the newer scene's models is found in the older one (by viewport,
// polygon data address and culling-node path; else the nearest with the same
// viewport and address: the same model reached through other nodes) and
// drawn between where it was and where it is: its matrix blended (position
// and scale straight, rotation along the shortest turn) and its polygons
// moved by the blended matrix times the inverse of the newer one, which put
// them where they are. Models not found (another model each frame: a
// flipbook, a wheel's blur, a bird's wings) move as their nearest relative
// found in the culling-node tree does (a wheel with its car, a bird with the
// scenery); with none (below the viewport's node), and models that moved
// too far in the frame (more than kMaxMove of their distance from the eye),
// they're drawn as in the newer scene. One that turned
// further than kMaxTurn degrees (a wheel spinning: which way can't be told)
// is moved with the rest but turned as in the newer scene, so it stays on
// its car. If fewer than half the newer scene's models were blended, it's a
// cut (the camera somewhere else): the newer scene as it is.
//
// Only the 3D's geometry moves: lighting, fog, colours, textures and level
// of detail are the newer frame's, as are the 2D layers.
#pragma once

#include "runtime/real3d_scene.hpp"

#include <cstddef>

namespace m3 {

struct InterpStats {
    std::size_t models = 0;    // the newer scene's
    std::size_t blended = 0;   // drawn between the two
    std::size_t unmatched = 0; // not in the older scene
    std::size_t followed = 0;  // of those, moved as their nearest relative found
    std::size_t moved = 0;     // moved too far: drawn as in the newer
    std::size_t spun = 0;      // turned too far: blended in place, the newer turn
    std::size_t nearest = 0;   // found as the nearest of the same model (not by its nodes)
    std::size_t ambiguous = 0; // of those, another nearly as near (within twice the distance)
    std::size_t compared = 0;  // models compared in those searches
    bool cut = false;          // the newer scene drawn as it is
};

// `out` becomes the scene at `t` from `a` (0) to `b` (1). Returns false (and
// `out` is `b`) at a cut, or when either is empty or block culled.
bool interpolate(const Real3dScene& a, const Real3dScene& b, float t, Real3dScene& out, InterpStats* stats = nullptr);

constexpr float kMaxMove = 0.75f;  // of the distance from the eye, in a frame
constexpr float kMaxTurn = 90.0f;  // degrees in a frame

// Smooth motion from the game's own camera (docs/smooth-motion.md, step 1):
// the camera between the two frames' (its eye's place straight, its turn the
// shortest), everything else where it is in the newer frame, seen through
// that camera. The viewports whose camera is the game's (their first node's
// matrix the game_camera's view, in b and a) are moved; the others (the radar,
// the HUD's 3D) are drawn as in b. No guessing which model is which.
//
// The camera shake (it lifts the eye) is taken out before blending and the
// newer frame's put back. A cut (out is b, false): either scene without the
// game's camera, the camera driven by something else (its driver changed), a
// win pose's shot starting, its eye (unshaken) moved more than kCameraJump or
// its view turned more than kCameraTurnJump in the frame. Measured over
// 150,000 frames of the whole game, every camera jump (an area or stage
// change, a scripted shot's instant command, a win pose's shot: 1.1 units and
// more) was over 1 unit and every other move under 0.6. The point it looks at
// isn't tested: a scripted command can move it along its line (the view stays).
struct CameraInterpStats {
    std::size_t viewports = 0; // moved by the blended camera
    std::size_t models = 0;    // in them
    float eye_move = 0;        // world units, without the shake
    float turn = 0;            // degrees, the first viewport's
    bool lagged = false;       // the newer scene drawn with the frame before's camera
    enum class Cut : uint8_t { none, no_camera, driver, shot, jump, turn, no_viewport } cut = Cut::none;
};
bool interpolate_camera(const Real3dScene& a, const Real3dScene& b, float t, Real3dScene& out,
                        CameraInterpStats* stats = nullptr);

constexpr float kCameraJump = 1.0f;      // world units in a frame
constexpr float kCameraTurnJump = 20.0f; // degrees in a frame

} // namespace m3

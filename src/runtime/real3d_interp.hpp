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
#include <memory>

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
//
// `models` (step 2): each model in those viewports is also found in the older
// scene, by its polygon data's address and its place in the world (the
// camera undone), in any of a's viewports with the game's camera (the stage's
// tree is drawn under one viewport one frame and its twin the next): the
// nearest within kModelMatch, with no other copy nearly as near (within
// twice its distance). Found, its place is blended (position straight, the
// turn the shortest: a spin keeps its size). Not found, a model of an
// animation (Real3dScene::Model::animation: a flipbook, like a gull's wing
// beats) is found as the nearest of that animation's models the same way: its
// place blended, its own (the newer) model drawn. Still not found (it
// appeared, moved too far) or copies alike (rain, splashes: which is which
// can't be told): drawn where it is in the newer scene. So is a model near a
// body the game put down in one jump (Real3dScene::placed: made, respawned).
// The characters (step 3): a model drawn on a body's bone (Real3dScene::
// bodies_before: the bones the scene was drawn with; its turn the bone's, its
// place near the bone's) is keyed by (body, bone, its address; failing that,
// by (body, address) when a had that model on the body once) and found in a
// by that key, no guessing: identical enemies side by side can't be mixed up.
// Found, it moves with its bone through the body's skeleton: each bone's turn
// and place relative to its parent blended, the chain rebuilt from the root,
// so joints stay joined however fast a limb swings. A shadow (the game draws
// many alike under each body: a model flat on the ground, not found or alike,
// near a body's bone) moves with the nearest bone across the ground, its
// height its own.
// A skin (a model in polygon RAM: the game builds a joint's skin each frame,
// its address packed by whoever draws) isn't matched: it moves as the limb on
// its bone (drawn with the same turn, bit for bit, within kSkinReach); none
// (the bone's own model left out), as the nearest model within kSkinReach;
// none, as in b.
struct CameraInterpStats {
    std::size_t viewports = 0; // moved by the blended camera
    std::size_t models = 0;    // in them
    std::size_t blended = 0;   // of those, found and their place blended
    std::size_t tied = 0;      // of those, a body's limb found by its body and bone
    std::size_t still = 0;     // found, in the same place
    std::size_t flipbook = 0;  // another model of its animation found where it was: its place blended
    std::size_t newer = 0;     // not found: as in the newer scene
    std::size_t ambiguous = 0; // copies alike: as in the newer scene
    std::size_t shadows = 0;   // flat under a body: moved with their bone across the ground
    std::size_t placed = 0;    // near a body put down in one jump: as in the newer scene
    std::size_t skins = 0;     // moved with their limb
    std::size_t skins_near = 0; // of those, no limb with their turn: with the nearest model
    std::size_t skins_shaped = 0; // of those, paired with a's on the same body and bone: their shape blended
    std::size_t skins_alone = 0; // no limb with their matrix: as in the newer scene
    float eye_move = 0;        // world units, without the shake
    float turn = 0;            // degrees, the first viewport's
    bool lagged = false;       // the newer scene drawn with the frame before's camera
    enum class Cut : uint8_t { none, no_camera, driver, shot, jump, turn, no_viewport } cut = Cut::none;
};
bool interpolate_camera(const Real3dScene& a, const Real3dScene& b, float t, Real3dScene& out,
                        CameraInterpStats* stats = nullptr, bool models = true);

// The same, the matching done once for a pair of frames and each picture
// between them only blended. a and b must stay as they are until the next
// prepare.
class CameraBlend {
public:
    CameraBlend();
    ~CameraBlend();
    CameraBlend(const CameraBlend&) = delete;
    CameraBlend& operator=(const CameraBlend&) = delete;
    bool prepare(const Real3dScene& a, const Real3dScene& b, bool models = true); // false: a cut
    void draw(float t, Real3dScene& out) const;                                  // after a cut: b
    const CameraInterpStats& stats() const;

private:
    struct Data;
    std::unique_ptr<Data> d_;
};

constexpr float kCameraJump = 1.0f;      // world units in a frame
constexpr float kCameraTurnJump = 20.0f; // degrees in a frame
constexpr float kModelMatch = 3.0f;      // world units in a frame (the docks' birds fly 1.6)
constexpr float kModelSame = 0.01f;      // world units: the same place
constexpr float kMaxModelTurn = 120.0f;  // degrees in a frame (a helicopter's rotor turns 48)
constexpr float kPlacedRadius = 2.5f;    // world units round a body put down in one jump: its limbs, its shadow
constexpr float kSkinReach = 1.0f;       // a skin's limb: the nearest with its turn, this near (seen 0.01-0.2)
constexpr float kBoneTurn = 1e-3f;       // a model on a body's bone: its turn the bone's, this near (each entry)
constexpr float kBoneReach = 1.0f;       // and its place within this of the bone's
constexpr float kShadowReach = 1.5f;     // a shadow: a body's bone this near across the ground (seen to 1.4)
constexpr float kShadowFlat = 0.99f;     // and its up axis this near straight up

} // namespace m3

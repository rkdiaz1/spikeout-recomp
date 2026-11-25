// The Real3D's scene for one frame, decoded from its memory (m3::Real3d):
// the viewports, and every polygon to draw with its vertices in eye space
// and its attributes, in drawing order. Both renderers draw from it: the
// software one (m3::Real3dRenderer) and the GPU one.
//
// Real3dSceneBuilder walks the viewports and their culling-node trees
// (matrices, frustum culling, level of detail) and decodes each model's
// polygons (colour, texture, lighting and drawing-order flags), dropping
// those facing away. The formats follow Supermodel's New3D (Src/Graphics/
// New3D: New3D.cpp, PolyHeader.h, R3DShaderCommon.h) and its notes; the code
// is ours. See real3d_render.hpp for the memory layout.
#pragma once

#include "runtime/real3d.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace m3 {

struct Mat4 {
    std::array<float, 16> m{}; // column-major: m[col * 4 + row]
    static Mat4 identity();
    Mat4 operator*(const Mat4& b) const;
};

struct Real3dScene {
    struct Viewport {
        int priority = 0;
        float x = 0, y = 0, width = 0, height = 0; // pixels, y from the top
        float left = -1, right = 1, bottom = -1, top = 1;
        float cota = 1; // word 3, for texture levels
        std::array<float, 3> sun{};
        float sun_intensity = 0, ambient = 0;
        // Fog: colour (times fog ambient), density per unit of distance, start;
        // the colour as set and the attenuation, for the spotlight on fog.
        std::array<float, 3> fog_colour{}, fog_colour_set{};
        float fog_density = 0, fog_start = 0, fog_attenuation = 0;
        // Spotlight: an ellipse on the screen (centre, radii in pixels) lit
        // from a distance on, falling off over its extent; its colours.
        float spot_x = 0, spot_y = 0, spot_rx = 1, spot_ry = 1, spot_start = 0, spot_extent = 0;
        std::array<float, 3> spot_colour{}, spot_fog_colour{};
        bool spot = false; // has a start distance
        // Scroll and ambient fog: the fog drawn over the 2D layers behind the
        // 3D (word 0x20 low byte; 0x24 low byte the scroll attenuation; 0x25
        // bits 16-23 the fog ambient).
        float scroll_fog = 0, scroll_attenuation = 0, fog_ambient = 1;
        // Smooth motion: the matrix of the first culling node under the
        // viewport (its camera, on Model 3 games: the world to eye space,
        // with the eye's axes turned), its index in the matrix table (-1:
        // none) and that entry's 12 words as stored (translation, then the
        // rotation's rows), to tell the game's camera by.
        int camera_index = -1;
        Mat4 camera;
        std::array<uint32_t, 12> camera_words{};
    };

    // A vertex in eye space (z < 0 in front), with its texture position (in
    // texels), fixed shade and lighting normal.
    struct Vertex {
        float x, y, z;
        float u, v;
        float shade;
        float nx, ny, nz;
    };

    // A polygon's attributes, the same at all its vertices.
    struct Polygon {
        float r, g, b, alpha;
        float fog; // intensity, 0 = none
        // Lighting: off for luminous polygons; fixed shading (the shade in
        // place of the sun's angle); specular, its strength and shininess
        // (0-3), smooth shading.
        bool lit, fixed_shading, specular, smooth;
        float specular_value;
        int shininess;
        uint16_t viewport; // index into viewports
        bool textured, contour, texture_alpha; // contour: alpha test (cut-out)
        // Drawing order: translucent (polygon or node alpha), which of the two
        // translucency layers, high priority (over the rest of its priority),
        // layered (a decal, after the rest of its viewport).
        bool poly_alpha, high_priority, layered;
        uint8_t trans_layer;
        int format;
        // Texture: position in its page, size, wrap per axis (0 repeat, 1
        // clamp within the texture, 2 mirror), mipmap level at 1/w = 1 and the
        // last level.
        uint32_t tx, ty, tw, th, page_y;
        uint8_t wrap_u, wrap_v;
        float lod_base;
        int max_level;
        // Microtexture: a 128x128 detail texture in the other page, blended in
        // close up (id, finest scale); inverted: the texture's colours inverted.
        bool micro, inverted;
        int micro_id, micro_min_lod;
        bool translator; // colour through the translator map (for reports)
        uint32_t address; // of its header, for the probe
        int priority;
        uint32_t first_vertex = 0; // into vertices
        uint8_t vertex_count = 3;  // 3, or 4: a quad
    };

    // Polygons in drawing order: priority by priority, viewport by viewport.
    std::vector<Viewport> viewports;
    std::vector<Polygon> polygons;
    std::vector<Vertex> vertices;

    // Each model drawn, in drawing order, with what smooth motion needs to
    // find it again in the next frame and move it between the two: its
    // polygon data's address, a hash of the culling nodes above it (its place
    // in the viewport's tree: one model drawn by several nodes is several
    // models), its viewport, its matrix (model to eye space: its polygons'
    // vertices are already through it) and its polygons.
    // Its ancestors: the path's hash at each culling node down to it (the
    // viewport's first), as far as kAncestors deep; a model not found in the
    // other frame moves with the one found nearest it in the tree (a wheel
    // with its car).
    // number: the game's own number for the model, when the game says which
    // culling node each model of its starts at (set_model_nodes: the nearest
    // such node above it), else kNoModel; animation: filled by the game's
    // code (0: none), the same for the models of one animation (a flipbook).
    static constexpr int kAncestors = 24;
    static constexpr uint32_t kNoModel = 0xFFFFFFFFu;
    struct Model {
        uint32_t address = 0, path = 0;
        uint16_t viewport = 0;
        Mat4 matrix;
        uint32_t first_polygon = 0, polygon_count = 0;
        std::array<uint32_t, kAncestors> ancestors{};
        uint8_t depth = 0; // ancestors in use
        uint32_t number = kNoModel;
        uint32_t animation = 0;
    };
    std::vector<Model> models;
    bool block_culled = false; // the Real3D's block culling was on: white, no scene
    bool fog_over_nothing = false; // scroll fog with no polygons too (Supermodel's; background_fog)

    // Smooth motion: the game's camera after the frame the scene was built
    // from, read from its RAM by the game's own code (games/spikeofe_camera:
    // nothing set by the builder, cleared by it). view: the camera's matrix
    // as the game stores it (its viewports' camera_words hold the same
    // floats); eye, look: where it is and the point it looks at, without the
    // shake; shake: how far the shake lifted the eye for the view; driver:
    // what moves it (a change: a cut); shot_timer, shot: a win pose's shots
    // (a new one: a cut).
    struct GameCamera {
        bool known = false;
        std::array<uint32_t, 12> view{};
        std::array<float, 3> eye{}, look{}; // without the shake
        float shake = 0;                    // the shake added to the eye's height (and the look-at's) for the view
        uint32_t driver = 0;
        uint16_t shot_timer = 0; // a win pose's shots: kShotOn while one runs
        uint8_t shot = 0;        // and its index
        static constexpr uint16_t kShotOn = 640;
        bool operator==(const GameCamera&) const = default;
    };
    // The 3D drawn is usually made with the camera of the same frame, but in
    // some places (seen after an area change) with the frame before's: both
    // are kept, and smooth motion takes the one the viewports hold.
    GameCamera game_camera, game_camera_before;
    // Smooth motion: where the game put a body down in one jump since the
    // frame before (made, lifted, respawned, snapped back), in the world, as
    // its code reads them (games/spikeofe_camera); cleared by the builder.
    std::vector<std::array<float, 3>> placed;
    // Smooth motion: the game's bodies (players, enemies) with their bones'
    // matrices in the world (the game's layout: the turn column by column,
    // then the place), as read after this scene's frame and after the one
    // before (the 3D draws bodies a frame late; which one a model was drawn
    // with is told by comparing); cleared by the builder.
    static constexpr int kGameBones = 20; // the root, then the 19 drawn
    struct GameBody {
        uint8_t slot = 0;
        std::array<std::array<float, 12>, kGameBones> bones{};
        bool operator==(const GameBody&) const = default;
    };
    std::vector<GameBody> bodies, bodies_before;
    // The game's effects (sparks, dust, trails: each drawn turned to the camera, games/spikeofe_camera), this read's
    // and the one before: the record's index (of the game's list), kind, frames left, its place, its own heading (a
    // 16-bit angle) and how far it's drawn toward the eye (its kind's).
    struct GameEffect {
        uint8_t index = 0, kind = 0, left = 0;
        uint16_t heading = 0;
        std::array<float, 3> place{};
        float pull = 0;
        bool operator==(const GameEffect&) const = default;
    };
    std::vector<GameEffect> effects, effects_before;

    struct Stats {
        uint32_t viewports = 0, nodes = 0, models = 0, polygons = 0;
        uint32_t models_in_polygon_ram = 0, hidden_nodes = 0, lod_rejected = 0, frustum_culled = 0, faded_out = 0;
        uint32_t spotlights = 0; // viewports with the spotlight on
    } stats;

    // Fog over the 2D layers behind the 3D, drawn before the scene so the
    // scene covers it (Supermodel's DrawAmbientFog and DrawScrollFog, its
    // "best guess at the logic based upon what games are doing"; Spikeout
    // sets scroll fog on one viewport of a priority):
    //  - ambient fog: in the lowest priority with viewports, the lowest fog
    //    ambient among those with fog (a density or start) and no scroll fog
    //    darkens its viewport: black at 1 - ambient;
    //  - scroll fog: the highest scroll fog value of the lowest priority that
    //    has one fills its viewport with its fog colour (times the fog
    //    ambient, plus the spotlight on fog) at that alpha, if some viewport
    //    with the same fog colour has fog or a spotlight on fog, and (unless
    //    fog_over_nothing) the frame has polygons to draw. After the staff
    //    credits the game leaves their viewports' scroll fog (orange, 0.65)
    //    set through the ranking, which has no 3D: Supermodel's rule covers
    //    the ranking with it, a real cabinet doesn't (the owner, 2026-10-29).
    //    The only frames in the game the rule changes (one player's run to
    //    the end, attract mode; docs/fidelity.md).
    struct BackgroundFog {
        int ambient = -1; // viewport darkened (-1: none), and by how much
        float ambient_alpha = 0;
        int scroll = -1; // viewport filled with scroll fog (-1: none)
        int light = -1;  // the viewport whose fog ambient and spotlight colour it
        float attenuation = 0; // of the spotlight on fog
        bool spot = false;     // the spotlight shows on it
    };
    BackgroundFog background_fog() const;

    // Whether a quad (its four vertices) is flat with its attributes linear
    // across it: then mean value coordinates draw it exactly as its two
    // triangles, and only other quads need drawing whole (--quads).
    static bool quad_is_linear(const Vertex* e);

    // The viewports as read, for reports.
    struct ViewportInfo {
        int priority;
        float x, y, width, height;
        uint32_t addr, eye, models;
        bool disabled;
        uint32_t spot_words[5]; // 0x1D-0x21: the spotlight
    };
    std::vector<ViewportInfo> viewport_info;
};

class Real3dSceneBuilder {
public:
    // Decodes the frame in `mem` into `scene` (its old contents replaced).
    void build(const Real3d& mem, Real3dScene& scene);

    // Widescreen (Supermodel's WideScreen; vf3-recomp's too): the viewports
    // that cover the whole screen (x 0, at least 495 x 383) are drawn
    // `extend` screen pixels further each side, their view opened out as
    // far (the same vertical view) and their side culling planes with it, or
    // what's past the 4:3 sides would pop in and out. Their x then starts at
    // -extend. 0: the arcade's screen.
    void set_extend(int pixels) { extend_ = pixels; }
    int extend() const { return extend_; }
    // Scroll fog drawn when the frame has no polygons too, as Supermodel
    // (Real3dScene::background_fog; --fog-over-nothing).
    void set_fog_over_nothing(bool on) { fog_over_nothing_ = on; }
    // The culling nodes (24-bit addresses) where the game's models start, and
    // each one's number (Real3dScene::Model::number); null: none. Must outlive
    // the builds that use it.
    void set_model_nodes(const std::unordered_map<uint32_t, uint32_t>* nodes) { model_nodes_ = nodes; }

private:
    struct ModelRef {
        Mat4 matrix;
        uint32_t addr = 0;
        uint32_t number = Real3dScene::kNoModel;
        uint32_t path = 0; // the culling nodes above it, hashed (Real3dScene::Model)
        std::array<uint32_t, Real3dScene::kAncestors> ancestors{};
        uint8_t depth = 0;
        int tex_x = 0, tex_y = 0, page = 0; // texture offset and page flip of its node
        uint32_t colour_table = 0;
        float alpha = 1; // its nodes' fade (level of detail), times each polygon's
        float scale = 1; // its node's model scale: normals are divided by it
    };
    struct Attribs {
        int tex_x = 0, tex_y = 0, page = 0;
        bool no_culling = false;
        float model_scale = 1; // scales the nodes' cull and blend radii
        float alpha = 1;
    };

    void walk_viewports(uint32_t addr);
    void walk_pointer(uint32_t ptr, int depth);
    void walk_node(uint32_t addr, int depth);
    const uint32_t* enter_node(uint32_t addr, int depth);
    void draw_node(uint32_t addr, const uint32_t* node, int depth);
    void add_model(uint32_t addr);
    void multiply_matrix(uint32_t index);
    void multiply_inverse_matrix(uint32_t index);
    void reset_rotation();
    float culling_float(uint32_t addr) const;
    void decode_model(size_t viewport, const ModelRef& model); // its polygons into the scene

    // The models in the polygon ROM (VROM, from 0x100000), read once: each polygon's header and vertices as decoded
    // (real3d_scene.cpp). A copy of the builder starts its own (one each, so builders on different threads share
    // nothing).
    struct ModelCache;
    struct ModelCacheSlot {
        std::shared_ptr<ModelCache> cache;
        ModelCacheSlot() = default;
        ModelCacheSlot(const ModelCacheSlot&) {}
        ModelCacheSlot& operator=(const ModelCacheSlot&) { cache.reset(); return *this; }
    };
    ModelCacheSlot model_cache_;

    const Real3d* mem_ = nullptr;
    Real3dScene* scene_ = nullptr;
    std::vector<ModelRef> models_;
    std::vector<std::pair<size_t, size_t>> viewport_models_; // first model and count, by viewport
    Mat4 matrix_;
    Mat4 coord_sys_; // the eye's rotation, for nodes that reset theirs
    Attribs attribs_;
    uint32_t matrix_base_ = 0, lod_table_ = 0, colour_table_ = 0;
    uint32_t path_ = 0; // the culling nodes walked down through, hashed (FNV-1a of their addresses)
    std::vector<uint32_t> ancestors_; // path_ at each of them, the viewport's first
    int extend_ = 0; // set_extend
    bool fog_over_nothing_ = false; // set_fog_over_nothing
    const std::unordered_map<uint32_t, uint32_t>* model_nodes_ = nullptr; // set_model_nodes
    uint32_t model_number_ = Real3dScene::kNoModel;                       // the nearest above, walking down
    // The viewport's frustum planes (words 0x0C-0x13): left u, v; top u, w;
    // right u, v; bottom u, w.
    std::array<float, 8> planes_{};
    uint32_t visits_ = 0; // guards against loops in a broken scene
    std::vector<uint32_t> sibling_chain_; // walk_node's chains of siblings, nested ones on top
};

} // namespace m3

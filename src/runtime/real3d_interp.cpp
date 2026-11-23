#include "runtime/real3d_interp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace m3 {

namespace {

using Vec3 = std::array<float, 3>;
using Mat3 = std::array<Vec3, 3>; // columns

Vec3 column(const Mat4& m, int c) { return {m.m[c * 4], m.m[c * 4 + 1], m.m[c * 4 + 2]}; }
Vec3 translation(const Mat4& m) { return {m.m[12], m.m[13], m.m[14]}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 apply(const Mat3& l, const Vec3& v) {
    return {l[0][0] * v[0] + l[1][0] * v[1] + l[2][0] * v[2], l[0][1] * v[0] + l[1][1] * v[1] + l[2][1] * v[2],
            l[0][2] * v[0] + l[1][2] * v[1] + l[2][2] * v[2]};
}
Mat3 times(const Mat3& a, const Mat3& b) { return {apply(a, b[0]), apply(a, b[1]), apply(a, b[2])}; }

bool inverse(const Mat3& l, Mat3& out) {
    const Vec3 c0 = cross(l[1], l[2]), c1 = cross(l[2], l[0]), c2 = cross(l[0], l[1]);
    const float det = dot(l[0], c0);
    if (!(std::fabs(det) > 1e-12f)) return false;
    // The inverse's rows are the cross products over the determinant.
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[static_cast<size_t>(c)][static_cast<size_t>(r)] = (r == 0 ? c0 : r == 1 ? c1 : c2)[static_cast<size_t>(c)] / det;
    return true;
}

// A matrix's linear part as scale (each column's length) and a rotation
// (the columns made orthonormal); mirrored: its determinant was negative, the
// first column turned round to make a rotation.
struct Parts {
    Vec3 scale;
    std::array<float, 4> q; // x, y, z, w
    bool mirrored;
};

std::array<float, 4> quaternion(const Mat3& r) { // r[c][row]
    const float m00 = r[0][0], m11 = r[1][1], m22 = r[2][2];
    const float tr = m00 + m11 + m22;
    std::array<float, 4> q;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        q = {(r[1][2] - r[2][1]) / s, (r[2][0] - r[0][2]) / s, (r[0][1] - r[1][0]) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        q = {0.25f * s, (r[1][0] + r[0][1]) / s, (r[2][0] + r[0][2]) / s, (r[1][2] - r[2][1]) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        q = {(r[1][0] + r[0][1]) / s, 0.25f * s, (r[2][1] + r[1][2]) / s, (r[2][0] - r[0][2]) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        q = {(r[2][0] + r[0][2]) / s, (r[2][1] + r[1][2]) / s, 0.25f * s, (r[0][1] - r[1][0]) / s};
    }
    return q;
}

Mat3 rotation(const std::array<float, 4>& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {Vec3{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
            Vec3{2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
            Vec3{2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}};
}

bool decompose(const Mat3& l, Parts& out) {
    Mat3 r = l;
    for (int c = 0; c < 3; ++c) {
        out.scale[static_cast<size_t>(c)] = length(r[static_cast<size_t>(c)]);
        if (!(out.scale[static_cast<size_t>(c)] > 1e-12f)) return false;
    }
    // Gram-Schmidt: the columns made orthonormal, the first kept.
    auto normalize = [](Vec3& v) {
        const float n = length(v);
        if (n > 1e-12f)
            for (float& c : v) c /= n;
        return n > 1e-12f;
    };
    if (!normalize(r[0])) return false;
    const float along = dot(r[0], r[1]);
    for (size_t k = 0; k < 3; ++k) r[1][k] -= along * r[0][k];
    if (!normalize(r[1])) return false;
    const Vec3 z = cross(r[0], r[1]);
    out.mirrored = dot(z, r[2]) < 0;
    r[2] = z;
    if (out.mirrored) // a mirror: the first column turned round, and back at the end
        for (float& c : r[0]) c = -c;
    out.q = quaternion(r);
    return true;
}

std::array<float, 4> slerp(std::array<float, 4> a, const std::array<float, 4>& b, float t) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) { // the shorter way round
        for (float& c : a) c = -c;
        d = -d;
    }
    std::array<float, 4> q;
    if (d > 0.9995f) {
        for (size_t i = 0; i < 4; ++i) q[i] = a[i] + (b[i] - a[i]) * t;
    } else {
        const float angle = std::acos(std::clamp(d, -1.0f, 1.0f)), s = std::sin(angle);
        const float wa = std::sin((1 - t) * angle) / s, wb = std::sin(t * angle) / s;
        for (size_t i = 0; i < 4; ++i) q[i] = a[i] * wa + b[i] * wb;
    }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (float& c : q) c /= n;
    return q;
}

float turn_degrees(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    const float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    return 2.0f * std::acos(std::min(d, 1.0f)) * 57.2957795f;
}

Mat3 linear(const Mat4& m) { return {column(m, 0), column(m, 1), column(m, 2)}; }

uint64_t key(const Real3dScene::Model& m) {
    return static_cast<uint64_t>(m.viewport) << 48 ^ static_cast<uint64_t>(m.address) << 24 ^ m.path;
}
uint64_t place(const Real3dScene::Model& m) { return static_cast<uint64_t>(m.viewport) << 32 | m.address; }

} // namespace

bool interpolate(const Real3dScene& a, const Real3dScene& b, float t, Real3dScene& out, InterpStats* stats) {
    out = b;
    InterpStats s;
    s.models = b.models.size();
    auto done = [&](bool blended) {
        s.cut = !blended;
        if (stats) *stats = s;
        return blended;
    };
    if (a.block_culled || b.block_culled || a.models.empty() || b.models.empty()) return done(false);

    // The older scene's models by their key, and by viewport and address.
    std::unordered_map<uint64_t, int> by_key; // -1: several
    std::unordered_map<uint64_t, std::vector<int>> by_place;
    for (size_t i = 0; i < a.models.size(); ++i) {
        const auto [it, added] = by_key.emplace(key(a.models[i]), static_cast<int>(i));
        if (!added) it->second = -1;
        by_place[place(a.models[i])].push_back(static_cast<int>(i));
    }

    struct Move {
        size_t model;
        Mat3 l;
        Vec3 t;
    };
    std::vector<Move> moves;
    moves.reserve(b.models.size());
    std::vector<size_t> strays; // not found: moved with their nearest relative found
    for (size_t i = 0; i < b.models.size(); ++i) {
        const Real3dScene::Model& mb = b.models[i];
        const Vec3 tb = translation(mb.matrix);
        int found = -1;
        if (const auto it = by_key.find(key(mb)); it != by_key.end() && it->second >= 0) found = it->second;
        if (found < 0) // the same model reached through other nodes: the nearest
            if (const auto it = by_place.find(place(mb)); it != by_place.end()) {
                float best = 0, second = 0;
                for (int j : it->second) {
                    const Vec3 ta = translation(a.models[static_cast<size_t>(j)].matrix);
                    const Vec3 d{ta[0] - tb[0], ta[1] - tb[1], ta[2] - tb[2]};
                    const float far = dot(d, d);
                    if (found < 0 || far < best) {
                        second = found < 0 ? 0 : best;
                        best = far;
                        found = j;
                    } else if (second == 0 || far < second) {
                        second = far;
                    }
                }
                s.compared += it->second.size();
                ++s.nearest;
                if (it->second.size() > 1 && second < 4 * best) ++s.ambiguous;
            }
        if (found < 0) {
            ++s.unmatched;
            strays.push_back(i);
            continue;
        }
        const Real3dScene::Model& ma = a.models[static_cast<size_t>(found)];
        const Vec3 ta = translation(ma.matrix);
        const Vec3 d{tb[0] - ta[0], tb[1] - ta[1], tb[2] - ta[2]};
        const Mat3 la = linear(ma.matrix), lb = linear(mb.matrix);
        Parts pa{}, pb{};
        Mat3 lb_inverse{};
        if (length(d) > kMaxMove * std::max(length(ta), length(tb)) || !decompose(la, pa) || !decompose(lb, pb) ||
            pa.mirrored != pb.mirrored || !inverse(lb, lb_inverse)) {
            ++s.moved;
            continue;
        }
        // The matrix between: position and scale straight, rotation the
        // shortest turn. Turned too far to tell which way (a wheel spinning):
        // the newer rotation, moved with the rest (else it'd lag its car).
        const bool spun = turn_degrees(pa.q, pb.q) > kMaxTurn;
        if (spun) ++s.spun;
        Mat3 lt = rotation(spun ? pb.q : slerp(pa.q, pb.q, t));
        if (pa.mirrored)
            for (float& c : lt[0]) c = -c;
        for (size_t c = 0; c < 3; ++c)
            for (float& v : lt[c]) v *= pa.scale[c] + (pb.scale[c] - pa.scale[c]) * t;
        const Vec3 tt{ta[0] + d[0] * t, ta[1] + d[1] * t, ta[2] + d[2] * t};
        // Its polygons, there in the newer scene, moved by (between) x (newer)^-1.
        const Mat3 l = times(lt, lb_inverse);
        const Vec3 lb_t = apply(l, tb);
        moves.push_back({i, l, {tt[0] - lb_t[0], tt[1] - lb_t[1], tt[2] - lb_t[2]}});
    }
    s.blended = moves.size();
    if (s.blended * 2 < s.models) return done(false); // a cut: the newer scene as it is

    // Models not found (another model each frame: a flipbook's, a wheel's
    // blur, a bird's wings, a gate's pieces) move as the one found nearest
    // them in the tree does (below the viewport's own node): else, drawn
    // where they are while all round them is drawn back along the way,
    // they'd jump between the two from picture to picture (ghosts).
    if (!strays.empty()) {
        std::unordered_map<uint64_t, size_t> by_ancestor; // (viewport, an ancestor's hash) -> a move under it
        for (size_t k = 0; k < s.blended; ++k) {
            const Real3dScene::Model& model = b.models[moves[k].model];
            for (int level = 1; level < model.depth; ++level)
                by_ancestor.emplace(static_cast<uint64_t>(model.viewport) << 32 | model.ancestors[static_cast<size_t>(level)], k);
        }
        for (size_t i : strays) {
            const Real3dScene::Model& model = b.models[i];
            for (int level = model.depth - 1; level >= 1; --level) {
                const auto it =
                    by_ancestor.find(static_cast<uint64_t>(model.viewport) << 32 | model.ancestors[static_cast<size_t>(level)]);
                if (it == by_ancestor.end()) continue;
                moves.push_back({i, moves[it->second].l, moves[it->second].t});
                ++s.followed;
                break;
            }
        }
    }

    for (const Move& move : moves) {
        const Real3dScene::Model& model = b.models[move.model];
        for (uint32_t p = model.first_polygon; p < model.first_polygon + model.polygon_count; ++p) {
            const Real3dScene::Polygon& poly = out.polygons[p];
            for (uint32_t k = 0; k < poly.vertex_count; ++k) {
                Real3dScene::Vertex& v = out.vertices[poly.first_vertex + k];
                const Vec3 pos = apply(move.l, {v.x, v.y, v.z}), n = apply(move.l, {v.nx, v.ny, v.nz});
                v.x = pos[0] + move.t[0];
                v.y = pos[1] + move.t[1];
                v.z = pos[2] + move.t[2];
                v.nx = n[0];
                v.ny = n[1];
                v.nz = n[2];
            }
        }
    }
    return done(true);
}

namespace {

// Whether a viewport's camera (its first node's matrix, as the Real3D holds
// it: the translation, then the rotation's rows) is the game's (as the game
// stores it: the rotation column-major, then the translation): the same
// floats, bit for bit.
bool is_game_camera(const Real3dScene::Viewport& vp, const Real3dScene::GameCamera& camera) {
    if (vp.camera_index < 0 || !camera.known) return false;
    const auto& w = vp.camera_words;
    const auto& v = camera.view;
    for (size_t i = 0; i < 3; ++i)
        if (w[i] != v[9 + i]) return false;
    for (size_t r = 0; r < 3; ++r) // (row r, column c) of the rotation: w's row-major, v's column-major
        for (size_t c = 0; c < 3; ++c)
            if (w[3 + r * 3 + c] != v[c * 3 + r]) return false;
    return true;
}

// A rigid matrix's parts for blending a camera: its linear part's turn and
// scale, and the eye's place in the world (-L^-1 t).
struct CameraParts {
    Parts parts;
    Vec3 eye;
};
bool camera_parts(const Mat4& m, CameraParts& out) {
    const Mat3 l = linear(m);
    Mat3 inv{};
    if (!decompose(l, out.parts) || !inverse(l, inv)) return false;
    const Vec3 e = m3::apply(inv, translation(m));
    out.eye = {-e[0], -e[1], -e[2]};
    return true;
}

float distance(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// x -> l x + t
struct Affine {
    Mat3 l;
    Vec3 t;
};
Affine affine(const Mat4& m) { return {linear(m), translation(m)}; }
Affine operator*(const Affine& a, const Affine& b) {
    const Vec3 at = m3::apply(a.l, b.t);
    return {times(a.l, b.l), {at[0] + a.t[0], at[1] + a.t[1], at[2] + a.t[2]}};
}
bool inverse(const Affine& a, Affine& out) {
    if (!inverse(a.l, out.l)) return false;
    const Vec3 t = m3::apply(out.l, a.t);
    out.t = {-t[0], -t[1], -t[2]};
    return true;
}

// The linear part made again from its parts: the turn q, then each column's
// scale (the first column turned round for a mirror).
Mat3 linear_from(const std::array<float, 4>& q, const Vec3& scale, bool mirrored) {
    Mat3 l = rotation(q);
    if (mirrored)
        for (float& c : l[0]) c = -c;
    for (size_t c = 0; c < 3; ++c)
        for (float& x : l[c]) x *= scale[c];
    return l;
}

bool same_linear(const Mat3& a, const Mat3& b) {
    for (size_t c = 0; c < 3; ++c)
        for (size_t r = 0; r < 3; ++r)
            if (std::fabs(a[c][r] - b[c][r]) > 1e-5f) return false;
    return true;
}

// A model in polygon RAM: built by the game each frame (a joint's skin),
// its address packed by whoever draws that frame.
bool built_each_frame(const Real3dScene::Model& model) { return (model.address & 0xFFFFFF) < 0x100000; }

// Turned to face the eye (a billboard: the game's effects, rebuilt each frame from the camera, only rolled about
// the view axis): in eye space its z axis along the view axis and its x and y across it, each within kBillboardFace.
bool facing_eye(const Real3dScene::Model& model) {
    const auto& m = model.matrix.m;
    auto across = [&](size_t column, size_t row) { // that axis's part along a row, as a share of its length
        const float x = m[column * 4], y = m[column * 4 + 1], z = m[column * 4 + 2];
        const float n = std::sqrt(x * x + y * y + z * z);
        return n > 1e-12f ? std::fabs(m[column * 4 + row]) / n : 1.0f;
    };
    return across(2, 0) < kBillboardFace && across(2, 1) < kBillboardFace && across(0, 2) < kBillboardFace &&
           across(1, 2) < kBillboardFace;
}

// The same turn (and scale), bit for bit: drawn on the same bone.
bool same_linear_bits(const Mat4& a, const Mat4& b) {
    for (int c = 0; c < 3; ++c)
        if (std::memcmp(&a.m[static_cast<size_t>(c * 4)], &b.m[static_cast<size_t>(c * 4)], 3 * sizeof(float)) != 0) return false;
    return true;
}

// Which body and bone a model is drawn on: the bone (of the bodies the scene
// was drawn with, read the frame before, or failing that this frame's) with
// its turn (within kBoneTurn, the game's layout: column by column) nearest
// its place (within kBoneReach). kNoKey: none.
constexpr uint64_t kNoKey = ~0ull;
uint64_t body_key(const Real3dScene& scene, const Affine& place) {
    if (scene.bodies.empty() && scene.bodies_before.empty()) return kNoKey;
    Parts parts{};
    if (!decompose(place.l, parts) || parts.mirrored) return kNoKey;
    const Mat3 r = rotation(parts.q);
    for (const auto* bodies : {&scene.bodies_before, &scene.bodies}) {
        uint64_t key = kNoKey;
        float nearest = kBoneReach;
        for (const Real3dScene::GameBody& body : *bodies)
            for (size_t bone = 0; bone < body.bones.size(); ++bone) {
                const auto& m = body.bones[bone];
                const float dt = distance(place.t, {m[9], m[10], m[11]});
                if (dt >= nearest) continue;
                bool same = true;
                for (size_t c = 0; c < 3 && same; ++c)
                    for (size_t row = 0; row < 3 && same; ++row) same = std::fabs(r[c][row] - m[c * 3 + row]) < kBoneTurn;
                if (!same) continue;
                nearest = dt;
                key = uint64_t{body.slot} << 40 | uint64_t{bone} << 32;
            }
        if (key != kNoKey) return key;
    }
    return kNoKey;
}

// The bodies paired between two frames (b's slot -> a's): each of b's with a's whose root is nearest, within
// kBodyPair, one each, the same record preferred by kBodySameRecord. Not simply by slot: the game can swap two
// bodies' records between frames (two identical enemies side by side, seen at stage 1's camp); paired by slot,
// each was blended from the other's place and both were drawn in the middle, one of them seeming to vanish.
std::unordered_map<uint8_t, uint8_t> pair_bodies(const Real3dScene& a, const Real3dScene& b) {
    struct Candidate {
        float d;
        size_t j, k;
    };
    std::vector<Candidate> candidates;
    for (size_t j = 0; j < b.bodies_before.size(); ++j)
        for (size_t k = 0; k < a.bodies_before.size(); ++k) {
            const auto &rb = b.bodies_before[j].bones[0], &ra = a.bodies_before[k].bones[0];
            const float d = distance({rb[9], rb[10], rb[11]}, {ra[9], ra[10], ra[11]});
            if (d >= kBodyPair) continue;
            candidates.push_back({d - (b.bodies_before[j].slot == a.bodies_before[k].slot ? kBodySameRecord : 0.0f), j, k});
        }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) { return x.d < y.d; });
    std::vector<bool> b_used(b.bodies_before.size()), a_used(a.bodies_before.size());
    std::unordered_map<uint8_t, uint8_t> a_slot_of;
    for (const Candidate& c : candidates) {
        if (b_used[c.j] || a_used[c.k]) continue;
        b_used[c.j] = a_used[c.k] = true;
        a_slot_of[b.bodies_before[c.j].slot] = a.bodies_before[c.k].slot;
    }
    return a_slot_of;
}

// A key of b's (body_key) as a's: its body's slot in a (pair_bodies); kNoKey if the body has no pair.
uint64_t key_in_a(uint64_t key, const std::unordered_map<uint8_t, uint8_t>& a_slot_of) {
    if (key == kNoKey) return kNoKey;
    const auto it = a_slot_of.find(static_cast<uint8_t>(key >> 40));
    if (it == a_slot_of.end()) return kNoKey;
    return (key & ~(0xFFull << 40)) | uint64_t{it->second} << 40;
}

// A shadow: flat on the ground (its up axis straight up) near a body's bone
// across the ground. Its move: that bone's across the ground from a to b (the
// same body, paired by pair_bodies, and bone), the height its own. False: not
// one, or the body isn't in a.
bool shadow_move(const Real3dScene& a, const Real3dScene& b, const Affine& place, Vec3& move,
                 const std::unordered_map<uint8_t, uint8_t>& a_slot_of) {
    Vec3 up = place.l[1];
    if (const float n = length(up); n > 1e-12f)
        for (float& c : up) c /= n;
    if (std::fabs(up[1]) < kShadowFlat) return false;
    const Real3dScene::GameBody* body = nullptr;
    size_t bone = 0;
    float nearest = kShadowReach;
    for (const Real3dScene::GameBody& candidate : b.bodies_before)
        for (size_t k = 0; k < candidate.bones.size(); ++k) {
            const auto& m = candidate.bones[k];
            if (const float across = std::hypot(place.t[0] - m[9], place.t[2] - m[11]); across < nearest) {
                nearest = across;
                body = &candidate;
                bone = k;
            }
        }
    if (!body) return false;
    const auto paired = a_slot_of.find(body->slot);
    if (paired == a_slot_of.end()) return false;
    for (const Real3dScene::GameBody& before : a.bodies_before)
        if (before.slot == paired->second) {
            const auto &was = before.bones[bone], &is = body->bones[bone];
            move = {was[9] - is[9], 0, was[11] - is[11]};
            return true;
        }
    return false;
}

} // namespace

// The bones' chain, by the RAM's bone order (the root, then the drawn ones:
// chest, head, left armpit, shoulder, arm, hand, the right's, waist, right
// thigh, knee, ankle, toe, the left's): each one's parent, -1 the root.
// Every parent comes before its children.
constexpr std::array<int, Real3dScene::kGameBones> kBoneParent = {-1, 0, 1, 1, 3, 4, 5, 1, 7, 8, 9, 0, 11, 12, 13, 14, 11, 16, 17, 18};

// A bone's matrix (the game's layout: the turn column by column, then the place) as an affine.
Affine bone_affine(const std::array<float, 12>& m) {
    return {Mat3{Vec3{m[0], m[1], m[2]}, Vec3{m[3], m[4], m[5]}, Vec3{m[6], m[7], m[8]}}, Vec3{m[9], m[10], m[11]}};
}

struct CameraBlend::Data {
    const Real3dScene* a = nullptr;
    const Real3dScene* b = nullptr;
    bool blend = false; // false: a cut, out is b
    CameraInterpStats stats;
    struct View {
        bool moved = false;
        CameraParts pa{}, pb{};
        float shake_a = 0, shake_b = 0;
        Mat3 lb_inverse{};
        Vec3 tb{};
        Affine view{}, unview{}; // b's camera, and undone
    };
    std::vector<View> views; // by b's viewport
    // Each of b's models: through the camera only, its place blended, or as
    // another model (a skin with its limb).
    // A body in both frames: its bones blended through the chain (each turn
    // relative to its parent, then placed from the parent's blended frame), so
    // its joints stay joined however far a limb swings in a frame.
    struct Skeleton {
        std::array<Affine, Real3dScene::kGameBones> b_inverse{}; // b's bones, undone
        std::array<Parts, Real3dScene::kGameBones> pa{}, pb{};   // each bone's turn relative to its parent, a and b
        std::array<Vec3, Real3dScene::kGameBones> ta{}, tb{};    // and its place in the parent's frame
        bool ok = false;
    };
    std::vector<Skeleton> skeletons;
    std::unordered_map<uint8_t, size_t> skeleton_of; // by body slot
    // billboard: facing the eye; its turn kept as in b (facing the blended camera too), its place (ta to tb in the
    // world) seen through the blended camera
    enum class Kind : uint8_t { camera, blend, follow, shift, bone, billboard };
    struct Plan {
        Kind kind = Kind::camera;
        uint32_t follow = 0;
        uint32_t skeleton = 0, bone = 0; // Kind::bone: drawn on that body's bone
        Vec3 shift{}; // a shadow: its move in the world from b to a (its bone's across the ground)
        // A skin: each vertex's way (in b's eye space) to where a's skin had it, in the skin's own frame (its shape
        // in a); empty: not paired.
        std::vector<Vec3> shape;
        Parts pa{}, pb{};
        Vec3 ta{}, tb{};
        Affine place_inverse{};
        // Kind::billboard, its game effect found (effect_exact): rebuilt as the game draws it, from the in-between
        // camera: its record's place (ta to tb), pulled `pull` toward the eye, its roll its own heading (degrees, a
        // to b) less the camera's; anchor_b: that pulled point in b's eye space, what it's turned about.
        bool effect_exact = false;
        float pull = 0, heading_a = 0, heading_b = 0;
        float spin_step = 0; // its spin's change from a to b (degrees; 0 if a didn't draw it)
        // its piece's own offset (from anchor_b, in the effect's turned frame) back to a's, put in b's eye space
        // (a piece that moves out as it ages, kind 18's; 0 if a didn't draw it)
        Vec3 offset_back{};
        Vec3 anchor_b{};
    };
    std::vector<Plan> plans;
};

namespace {

// The heading of a camera's view (world -> eye, its linear part): its forward ((0, 0, -1) in eye space) across the
// ground, in degrees, as the game's effects take it (atan2(x, z)).
float camera_heading(const Mat3& view) {
    return static_cast<float>(std::atan2(-view[0][2], -view[2][2]) * 180.0 / 3.14159265358979);
}

// `place` drawn `pull` toward `eye` (an effect: in front of the surface it's on).
Vec3 pulled(const Vec3& place, float pull, const Vec3& eye) {
    const Vec3 to{eye[0] - place[0], eye[1] - place[1], eye[2] - place[2]};
    const float n = length(to);
    if (n < 1e-12f) return place;
    return {place[0] + pull * to[0] / n, place[1] + pull * to[1] / n, place[2] + pull * to[2] / n};
}

// A turn of `degrees` about the view axis (eye space's z).
Mat3 roll_about_view(float degrees) {
    const float r = degrees * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
    return Mat3{Vec3{c, s, 0}, Vec3{-s, c, 0}, Vec3{0, 0, 1}};
}

// The difference b - a of two angles in degrees, the short way round (-180..180).
float angle_step(float a, float b) {
    float d = std::fmod(b - a, 360.0f);
    if (d > 180) d -= 360;
    if (d < -180) d += 360;
    return d;
}

// A model turned to the eye and the game's effect record it draws (the decompile's effect_draw_one): the record's
// place pulled its kind's pull toward the eye (`anchor`, in the world), the model a piece of it there or at its own
// offset from there in the effect's turned frame (mostly along its own up: 0.25, 0.81; a footstep's dust has two or
// three puffs, a little to the side too), within kEffectOffset; its roll in eye space the record's heading less the
// camera's, plus 180 degrees, plus `spin` (what some kinds add as they age). Of the records that near, the one whose
// offset is least to the side of its own up (the main piece's is straight up, a puff's a few hundredths off; another
// record's would be well off), of this read's (`now`) or the one before's (`before_read`). None: effect null.
struct FoundEffect {
    const Real3dScene::GameEffect* effect = nullptr;
    bool before_read = false;
    Vec3 anchor{};
    float spin = 0;
};
FoundEffect find_effect(const std::vector<Real3dScene::GameEffect>& now, const std::vector<Real3dScene::GameEffect>& before,
                        const Real3dScene::Model& model, const Affine& place, const Affine& view, const Vec3& eye) {
    FoundEffect found;
    const Mat3 lm = linear(model.matrix);
    const float roll = static_cast<float>(std::atan2(lm[0][1], lm[0][0]) * 180.0 / 3.14159265358979);
    const float camera = camera_heading(view.l);
    const Mat3 unroll = roll_about_view(-roll);
    float best = 1e30f;
    for (const bool before_read : {false, true})
        for (const Real3dScene::GameEffect& fx : before_read ? before : now) {
            const Vec3 q = pulled(fx.place, fx.pull, eye);
            if (distance(q, place.t) >= kEffectOffset) continue;
            const Vec3 own = m3::apply(unroll, m3::apply(view.l, {place.t[0] - q[0], place.t[1] - q[1], place.t[2] - q[2]}));
            const float aside = std::fabs(own[0]) + std::fabs(own[2]);
            if (aside >= best) continue;
            best = aside;
            found = {&fx, before_read, q, angle_step(static_cast<float>(fx.heading) * 360.0f / 65536.0f - camera + 180.0f, roll)};
        }
    return found;
}
// A record's identity from one read to the next: its index and frames left.
uint32_t effect_key(uint8_t index, uint8_t left) { return uint32_t{index} << 8 | left; }

} // namespace

CameraBlend::CameraBlend() : d_(std::make_unique<Data>()) {}
CameraBlend::~CameraBlend() = default;
const CameraInterpStats& CameraBlend::stats() const { return d_->stats; }

bool CameraBlend::prepare(const Real3dScene& a, const Real3dScene& b, bool models) {
    Data& d = *d_;
    d.a = &a;
    d.b = &b;
    d.blend = false;
    d.views.assign(b.viewports.size(), {});
    d.plans.assign(b.models.size(), {});
    CameraInterpStats& s = d.stats;
    s = {};
    auto done = [&](CameraInterpStats::Cut cut) {
        s.cut = cut;
        d.blend = cut == CameraInterpStats::Cut::none;
        return d.blend;
    };
    // The camera each scene's 3D was made with: its frame's or the frame before's.
    auto drawn_with = [](const Real3dScene& scene) -> const Real3dScene::GameCamera* {
        for (const Real3dScene::GameCamera* c : {&scene.game_camera, &scene.game_camera_before})
            for (const Real3dScene::Viewport& vp : scene.viewports)
                if (is_game_camera(vp, *c)) return c;
        return nullptr;
    };
    if (!a.game_camera.known || !b.game_camera.known || a.block_culled || b.block_culled)
        return done(CameraInterpStats::Cut::no_camera);
    const Real3dScene::GameCamera *pca = drawn_with(a), *pcb = drawn_with(b);
    if (!pca || !pcb) return done(CameraInterpStats::Cut::no_viewport);
    s.lagged = pcb == &b.game_camera_before;
    const Real3dScene::GameCamera &ca = *pca, &cb = *pcb;
    s.eye_move = distance(ca.eye, cb.eye);
    if (ca.driver != cb.driver) return done(CameraInterpStats::Cut::driver);
    if (cb.shot_timer == Real3dScene::GameCamera::kShotOn && (ca.shot_timer != cb.shot_timer || ca.shot != cb.shot))
        return done(CameraInterpStats::Cut::shot);
    if (s.eye_move > kCameraJump) return done(CameraInterpStats::Cut::jump);

    // Each of b's viewports with the game's camera, with the camera's in a:
    // a viewport of a at the same address with the game's camera, else the
    // first with it.
    std::vector<size_t> a_cameras;
    for (size_t v = 0; v < a.viewports.size(); ++v)
        if (is_game_camera(a.viewports[v], ca)) a_cameras.push_back(v);
    if (a_cameras.empty()) return done(CameraInterpStats::Cut::no_viewport);
    for (size_t v = 0; v < b.viewports.size(); ++v) {
        if (!is_game_camera(b.viewports[v], cb)) continue;
        size_t from = a_cameras.front();
        if (v < b.viewport_info.size())
            for (size_t k : a_cameras)
                if (k < a.viewport_info.size() && a.viewport_info[k].addr == b.viewport_info[v].addr) {
                    from = k;
                    break;
                }
        Data::View& view = d.views[v];
        const Mat4 &va = a.viewports[from].camera, &vb = b.viewports[v].camera;
        if (!camera_parts(va, view.pa) || !camera_parts(vb, view.pb) || view.pa.parts.mirrored != view.pb.parts.mirrored ||
            !inverse(linear(vb), view.lb_inverse) || !inverse(affine(vb), view.unview))
            continue;
        const float turn = turn_degrees(view.pa.parts.q, view.pb.parts.q);
        if (s.viewports == 0) s.turn = turn;
        if (turn > kCameraTurnJump) return done(CameraInterpStats::Cut::turn);
        view.shake_a = ca.shake;
        view.shake_b = cb.shake;
        view.tb = translation(vb);
        view.view = affine(vb);
        view.moved = true;
        ++s.viewports;
    }
    if (s.viewports == 0) return done(CameraInterpStats::Cut::no_viewport);
    done(CameraInterpStats::Cut::none);
    if (!models) {
        for (const Real3dScene::Model& model : b.models) s.models += model.viewport < d.views.size() && d.views[model.viewport].moved;
        return true;
    }

    // Each model's place in the world: its viewport's camera undone. The
    // older scene's, by polygon address, from every viewport with the game's
    // camera (the stage's tree is drawn under one viewport one frame and its
    // twin the next). Skins aren't matched (their addresses say nothing).
    std::vector<Affine> a_unview(a.viewports.size());
    std::vector<bool> a_known(a.viewports.size(), false);
    for (size_t v : a_cameras) a_known[v] = inverse(affine(a.viewports[v].camera), a_unview[v]);
    std::unordered_map<uint32_t, std::vector<Affine>> before; // by address: each copy's place
    std::unordered_map<uint32_t, std::vector<Affine>> before_animation; // by animation: its models' places
    for (const Real3dScene::Model& model : a.models)
        if (model.viewport < a_known.size() && a_known[model.viewport] && !built_each_frame(model)) {
            const Affine place = a_unview[model.viewport] * affine(model.matrix);
            before[model.address].push_back(place);
            if (model.animation) before_animation[model.animation].push_back(place);
        }
    // The nearest of `places` to `place`, and the second nearest's distance.
    auto nearest_of = [](const std::vector<Affine>* places, const Affine& place, float& d1, float& d2) {
        const Affine* nearest = nullptr;
        d1 = d2 = 1e30f;
        if (places)
            for (const Affine& p : *places) {
                const float dp = distance(p.t, place.t);
                if (dp < d1) {
                    d2 = d1;
                    d1 = dp;
                    nearest = &p;
                } else if (dp < d2) d2 = dp;
            }
        return nearest;
    };
    auto find = [](const std::unordered_map<uint32_t, std::vector<Affine>>& map, uint32_t key) {
        const auto it = map.find(key);
        return it == map.end() ? nullptr : &it->second;
    };
    auto placed_near = [&](const Vec3& p) {
        for (const auto& body : b.placed)
            if (distance(body, p) < kPlacedRadius) return true;
        return false;
    };

    // The characters: each model drawn on a body's bone keyed by (slot, bone)
    // and its own address; those in b are found in a by that key, no
    // guessing (identical enemies side by side included). A bone can carry
    // more than one model, so the address is part of the key; one whose model
    // changed (a head's or hand's swap) is found by its bone alone when a had
    // just one model on it.
    // (a model drawn twice in one place, as two detail levels cross-fade, is one key: the first kept)
    std::unordered_map<uint64_t, Affine> a_keyed;
    // by (slot, the model's address): the place and how many (a straight joint can give two bones the same turn,
    // so a model's bone can differ between the frames: then it's found by its body and model, if it's there once)
    std::unordered_map<uint64_t, std::pair<Affine, int>> a_body_models;
    constexpr uint64_t kSlotOnly = 0xFFull << 40;
    // by (slot, bone) alone: a model there (the last resort, its address aside: two identical enemies side by side can
    // swap their models' copies between frames, each drawn from the other's the next frame; matched by address, each
    // limb took the other enemy's place, both drawn in the middle and one seeming to vanish, seen at stage 1's camp.
    // A limb on its bone moves through the skeleton, so which of the bone's models it pairs with doesn't matter)
    std::unordered_map<uint64_t, Affine> a_on_bone;
    for (const Real3dScene::Model& model : a.models)
        if (model.viewport < a_known.size() && a_known[model.viewport] && !built_each_frame(model)) {
            const Affine place = a_unview[model.viewport] * affine(model.matrix);
            const uint64_t key = body_key(a, place);
            if (key == kNoKey) continue;
            a_keyed.emplace(key | (model.address & 0xFFFFFF), place);
            a_on_bone.emplace(key, place);
            const auto [it, added] = a_body_models.emplace((key & kSlotOnly) | (model.address & 0xFFFFFF), std::pair{place, 1});
            if (!added) ++it->second.second;
        }

    // a's effects as it drew them: each one's spin (find_effect), by its record (effect_key); and (tests) where a drew
    // it, with a's eye and camera
    std::unordered_map<uint32_t, float> a_effect_spins;
    struct DrawnEffect {
        Vec3 at{}, eye{}, own{}; // (own: the piece's offset in the effect's turned frame, which piece it is)
        Affine view{};
        float roll = 0;
    };
    std::unordered_map<uint32_t, std::vector<DrawnEffect>> a_effects_drawn;
    for (const Real3dScene::Model& model : a.models)
        if (model.viewport < a_known.size() && a_known[model.viewport] && facing_eye(model)) {
            const Affine& unview_a = a_unview[model.viewport];
            const Affine view_a = affine(a.viewports[model.viewport].camera);
            const FoundEffect fe = find_effect(a.effects, a.effects_before, model, unview_a * affine(model.matrix), view_a, unview_a.t);
            if (!fe.effect) continue;
            a_effect_spins[effect_key(fe.effect->index, fe.effect->left)] = fe.spin;
            const Mat3 lm = linear(model.matrix);
            const float roll_a = static_cast<float>(std::atan2(lm[0][1], lm[0][0]) * 180.0 / 3.14159265358979);
            const Vec3 at = translation(model.matrix), anchor = (view_a * Affine{Mat3{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}, fe.anchor}).t;
            a_effects_drawn[effect_key(fe.effect->index, fe.effect->left)].push_back(
                {at, unview_a.t, m3::apply(roll_about_view(-roll_a), {at[0] - anchor[0], at[1] - anchor[1], at[2] - anchor[2]}),
                 view_a, roll_a});
        }

    // The skeletons: each body drawn in both frames (paired by place: pair_bodies), its bones relative to their parents.
    const std::unordered_map<uint8_t, uint8_t> a_slot_of = pair_bodies(a, b);
    d.skeletons.clear();
    d.skeleton_of.clear();
    for (const Real3dScene::GameBody& body_b : b.bodies_before)
        for (const Real3dScene::GameBody& body_a : a.bodies_before) {
            const auto paired = a_slot_of.find(body_b.slot);
            if (paired == a_slot_of.end() || body_a.slot != paired->second) continue;
            Data::Skeleton sk;
            sk.ok = true;
            std::array<Affine, Real3dScene::kGameBones> inverse_a{};
            for (size_t k = 0; k < body_b.bones.size() && sk.ok; ++k) {
                const Affine bone_a = bone_affine(body_a.bones[k]), bone_b = bone_affine(body_b.bones[k]);
                if (!inverse(bone_b, sk.b_inverse[k]) || !inverse(bone_a, inverse_a[k])) {
                    sk.ok = false;
                    break;
                }
                const int parent = kBoneParent[k];
                const Affine local_a = parent < 0 ? bone_a : inverse_a[static_cast<size_t>(parent)] * bone_a;
                const Affine local_b = parent < 0 ? bone_b : sk.b_inverse[static_cast<size_t>(parent)] * bone_b;
                sk.ok = decompose(local_a.l, sk.pa[k]) && decompose(local_b.l, sk.pb[k]) && sk.pa[k].mirrored == sk.pb[k].mirrored;
                sk.ta[k] = local_a.t;
                sk.tb[k] = local_b.t;
            }
            if (sk.ok) {
                d.skeleton_of[body_b.slot] = d.skeletons.size();
                d.skeletons.push_back(sk);
            }
            break;
        }
    // A tied model (or skin) drawn on a body's bone moves as the bone does, through the chain.
    auto on_bone = [&](Data::Plan& plan, uint64_t key) {
        if (key == kNoKey) return false;
        const auto it = d.skeleton_of.find(static_cast<uint8_t>(key >> 40));
        if (it == d.skeleton_of.end()) return false;
        plan.kind = Data::Kind::bone;
        plan.skeleton = static_cast<uint32_t>(it->second);
        plan.bone = static_cast<uint32_t>(key >> 32 & 0xFF);
        return true;
    };

    // The limbs (anything drawn with a matrix a skin may share), by viewport and matrix.
    std::vector<size_t> skins;
    for (size_t i = 0; i < b.models.size(); ++i) {
        const Real3dScene::Model& model = b.models[i];
        if (model.viewport >= d.views.size() || !d.views[model.viewport].moved) continue;
        ++s.models;
        if (built_each_frame(model)) {
            skins.push_back(i);
            continue;
        }
        const Data::View& view = d.views[model.viewport];
        const Affine place = view.unview * affine(model.matrix);
        float d1 = 0, d2 = 0;
        const Affine* nearest = nullptr;
        static const bool no_billboards = std::getenv("SPIKEOUT_SMOOTH_NO_BILLBOARDS") != nullptr; // (tests: as before)
        if (!no_billboards && facing_eye(model)) {
            // An effect (the game turns each to the camera every frame): kept facing the blended camera. Its place:
            // b's, or blended from the frame before's if found there (by address, else its animation's flipbook;
            // one clearly nearest, neither put down in one jump): those that follow their owner.
            Data::Plan& plan = d.plans[i];
            plan.kind = Data::Kind::billboard;
            plan.ta = plan.tb = place.t;
            // Its record (find_effect): its place, pulled toward the eye; its own offset from there along its own
            // up only; its spin (the roll beyond its heading's rule). Its place, heading and spin in a: the same
            // record continuing (its kind, one frame older or the same) in the same read of a's, its spin measured
            // on a's own model of it when a drew one.
            if (const FoundEffect fe = find_effect(b.effects, b.effects_before, model, place, view.view, view.unview.t); fe.effect) {
                const Real3dScene::GameEffect* found = fe.effect;
                plan.effect_exact = true;
                plan.pull = found->pull;
                plan.ta = plan.tb = found->place;
                plan.heading_a = plan.heading_b = static_cast<float>(found->heading) * 360.0f / 65536.0f;
                plan.anchor_b = (view.view * Affine{Mat3{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}, fe.anchor}).t;
                // (the record as a drew it: the same index and kind, a frame older, or the same if it didn't age;
                // of whichever read a drew it from)
                const Real3dScene::GameEffect* before_fx = nullptr;
                const auto spin_a = [&] {
                    for (const uint8_t left : {static_cast<uint8_t>(found->left + 1), found->left})
                        if (const auto it = a_effect_spins.find(effect_key(found->index, left)); it != a_effect_spins.end()) return it;
                    return a_effect_spins.end();
                }();
                for (const uint8_t left : {static_cast<uint8_t>(found->left + 1), found->left}) {
                    for (const auto* list_a : {&a.effects, &a.effects_before})
                        for (const Real3dScene::GameEffect& fx : *list_a)
                            if (!before_fx && fx.index == found->index && fx.kind == found->kind && fx.left == left &&
                                distance(fx.place, found->place) <= kModelMatch)
                                before_fx = &fx;
                    if (before_fx) break;
                }
                if (before_fx) {
                    plan.ta = before_fx->place;
                    plan.heading_a = static_cast<float>(before_fx->heading) * 360.0f / 65536.0f;
                    s.billboards_moved += distance(plan.ta, plan.tb) > kModelSame;
                    if (spin_a != a_effect_spins.end()) {
                        plan.spin_step = angle_step(spin_a->second, fe.spin);
                        // the same piece in a (the nearest own offset) and its own offset there
                        if (const auto drawn = a_effects_drawn.find(spin_a->first); drawn != a_effects_drawn.end()) {
                            const Mat3 lb = linear(model.matrix);
                            const float roll_b = static_cast<float>(std::atan2(lb[0][1], lb[0][0]) * 180.0 / 3.14159265358979);
                            const Vec3 at_b = translation(model.matrix);
                            const Vec3 own_b = m3::apply(roll_about_view(-roll_b), {at_b[0] - plan.anchor_b[0], at_b[1] - plan.anchor_b[1],
                                                                                   at_b[2] - plan.anchor_b[2]});
                            const DrawnEffect* piece = &drawn->second.front();
                            for (const DrawnEffect& p : drawn->second)
                                if (distance(p.own, own_b) < distance(piece->own, own_b)) piece = &p;
                            plan.offset_back = m3::apply(roll_about_view(roll_b), {piece->own[0] - own_b[0], piece->own[1] - own_b[1],
                                                                                   piece->own[2] - own_b[2]});
                        }
                    }
                    // (tests: rebuilt for a's moment, from b's model, it lands where a drew it: a's place pulled
                    // toward a's eye, b's own offset turned by the roll's change)
                    static const bool check = std::getenv("SPIKEOUT_INTERP_DEBUG") != nullptr;
                    if (check && spin_a != a_effect_spins.end())
                        if (const auto drawn = a_effects_drawn.find(spin_a->first); drawn != a_effects_drawn.end()) {
                            const Vec3 at_b = translation(model.matrix);
                            const Vec3 offset_b{at_b[0] - plan.anchor_b[0], at_b[1] - plan.anchor_b[1], at_b[2] - plan.anchor_b[2]};
                            const Mat3 lm = linear(model.matrix);
                            const float roll_b = static_cast<float>(std::atan2(lm[0][1], lm[0][0]) * 180.0 / 3.14159265358979);
                            // (a's piece: the one at the nearest own offset, the same puff)
                            const Vec3 own_b = m3::apply(roll_about_view(-roll_b), offset_b);
                            const DrawnEffect* piece = &drawn->second.front();
                            for (const DrawnEffect& p : drawn->second)
                                if (distance(p.own, own_b) < distance(piece->own, own_b)) piece = &p;
                            const DrawnEffect& da = *piece;
                            const Vec3 anchor_a = (da.view * Affine{Mat3{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}},
                                                                     pulled(plan.ta, plan.pull, da.eye)}).t;
                            const Vec3 offset_a = m3::apply(roll_about_view(angle_step(roll_b, da.roll)),
                                                            {offset_b[0] + plan.offset_back[0], offset_b[1] + plan.offset_back[1],
                                                             offset_b[2] + plan.offset_back[2]});
                            const Vec3 predicted{anchor_a[0] + offset_a[0], anchor_a[1] + offset_a[1], anchor_a[2] + offset_a[2]};
                            std::printf("  effect check: kind %u, rebuilt for a %.5f from where a drew it (record %u left %u, %s; "
                                        "a's record left %u; roll b %.2f, a %.2f; spin step %.2f; anchor %.4f, offset %.4f)\n",
                                        found->kind, distance(predicted, da.at), found->index, found->left,
                                        fe.before_read ? "the read before" : "this read", before_fx->left, roll_b, da.roll,
                                        plan.spin_step, distance(anchor_a, da.at), length(offset_b));
                        }
                }
                ++s.billboards;
                ++s.billboards_exact;
                static const bool debug_found = std::getenv("SPIKEOUT_INTERP_DEBUG") != nullptr;
                if (debug_found)
                    std::printf("  effect found: record %u kind %u left %u (%s), moved %.4f, heading step %.3f, spin step %.3f%s%s%s\n",
                                found->index, found->kind, found->left, fe.before_read ? "the read before" : "this read",
                                distance(plan.ta, plan.tb), angle_step(plan.heading_a, plan.heading_b), plan.spin_step,
                                plan.heading_a == plan.heading_b && plan.ta == plan.tb && plan.spin_step == 0 ? " (as b)" : "",
                                before_fx ? "" : " (no record in a)", spin_a == a_effect_spins.end() ? " (a drew none)" : "");
                continue;
            }
            static const bool debug_effects = std::getenv("SPIKEOUT_INTERP_DEBUG") != nullptr;
            if (debug_effects) { // (tests: a model turned to the eye with no record that fits it)
                const Vec3 eye_b = view.unview.t;
                const Real3dScene::GameEffect* near_fx = nullptr;
                float near_d = 1e30f;
                for (const auto* list : {&b.effects, &b.effects_before})
                    for (const Real3dScene::GameEffect& fx : *list)
                        if (const float dq = distance(pulled(fx.place, fx.pull, eye_b), place.t); dq < near_d) near_d = dq, near_fx = &fx;
                if (near_fx)
                    std::printf("  effect not found: nearest record %u kind %u left %u, %.3f away\n", near_fx->index,
                                near_fx->kind, near_fx->left, near_d);
                else std::printf("  effect not found: no records\n");
            }
            nearest = nearest_of(find(before, model.address), place, d1, d2);
            if ((!nearest || d1 > kModelMatch) && model.animation)
                nearest = nearest_of(find(before_animation, model.animation), place, d1, d2);
            if (nearest && d1 <= kModelMatch && d2 >= 2 * d1 + kModelSame && !placed_near(place.t) &&
                !placed_near(nearest->t)) {
                plan.ta = nearest->t;
                s.billboards_moved += d1 > kModelSame;
            }
            ++s.billboards;
            continue;
        }
        bool flipbook = false, tied = false;
        const uint64_t key = body_key(b, place);
        if (const uint64_t key_a = key_in_a(key, a_slot_of); key_a != kNoKey) { // a body's limb: the same one
            if (const auto it = a_keyed.find(key_a | (model.address & 0xFFFFFF)); it != a_keyed.end()) nearest = &it->second;
            else if (const auto same = a_body_models.find((key_a & kSlotOnly) | (model.address & 0xFFFFFF));
                     same != a_body_models.end() && same->second.second == 1)
                nearest = &same->second.first;
            else if (const auto on_bone_a = a_on_bone.find(key_a); on_bone_a != a_on_bone.end())
                nearest = &on_bone_a->second;
            if (nearest) {
                d1 = distance(nearest->t, place.t);
                d2 = 1e30f;
                tied = true;
            }
        }
        if (!tied) nearest = nearest_of(find(before, model.address), place, d1, d2);
        if (!tied && (!nearest || d1 > kModelMatch) && model.animation) {
            // Another model of its animation (a flipbook's next frame) where it was: the place blended, this
            // frame's model drawn.
            nearest = nearest_of(find(before_animation, model.animation), place, d1, d2);
            flipbook = true;
        }
        Data::Plan& plan = d.plans[i];
        const bool missing = !nearest || d1 > kModelMatch, alike = !missing && d2 < 2 * d1 + kModelSame;
        Vec3 shift{};
        if (tied && !placed_near(place.t) && !placed_near(nearest->t) && on_bone(plan, key)) {
            ++s.blended; // a body's limb: through its skeleton
            ++s.tied;
        } else if ((missing || alike) && !tied && !placed_near(place.t) && shadow_move(a, b, place, shift, a_slot_of)) {
            plan.kind = Data::Kind::shift; // a shadow (many alike under a body): with its bone across the ground
            plan.shift = shift;
            ++s.shadows;
        } else if (missing) ++s.newer; // appeared, moved too far, not part of an animation
        else if (alike) ++s.ambiguous; // copies alike: which is which can't be told
        else if (d1 <= kModelSame && same_linear(nearest->l, place.l)) ++s.still;
        else if (placed_near(place.t) || placed_near(nearest->t)) ++s.placed; // put down in one jump
        else if (decompose(nearest->l, plan.pa) && decompose(place.l, plan.pb) && plan.pa.mirrored == plan.pb.mirrored &&
                 turn_degrees(plan.pa.q, plan.pb.q) <= kMaxModelTurn && inverse(place, plan.place_inverse)) {
            plan.kind = Data::Kind::blend;
            plan.ta = nearest->t;
            plan.tb = place.t;
            ++(flipbook ? s.flipbook : s.blended);
            s.tied += tied;
        } else ++s.newer;
        // Tests (SPIKEOUT_INTERP_DEBUG=1, with a short gamerun --interp-trace): each model on or near a body's bone
        static const bool debug = std::getenv("SPIKEOUT_INTERP_DEBUG") != nullptr;
        if (debug) {
            const uint64_t key_a_side = nearest ? body_key(a, *nearest) : kNoKey;
            if (key != kNoKey || key_a_side != kNoKey)
                std::printf("  model %zu at %06x: at %.2f %.2f %.2f, b's key %d/%d, %s, a's %.2f away (key %d/%d), "
                            "next %.2f; drawn as %d\n",
                            i, model.address & 0xFFFFFF, place.t[0], place.t[1], place.t[2],
                            key == kNoKey ? -1 : static_cast<int>(key >> 40), key == kNoKey ? -1 : static_cast<int>(key >> 32 & 0xFF),
                            tied ? "tied" : "not tied", d1, key_a_side == kNoKey ? -1 : static_cast<int>(key_a_side >> 40),
                            key_a_side == kNoKey ? -1 : static_cast<int>(key_a_side >> 32 & 0xFF), d2,
                            static_cast<int>(plan.kind));
        }
    }
    // a's skins by the body and bone they're drawn on (their matrix the
    // bone's), in drawing order, to pair with b's: the k-th on a bone with the
    // k-th, when their polygons are laid out alike (the game rebuilds each
    // skin every frame in the same layout, only its vertices moved).
    std::unordered_map<uint64_t, std::vector<size_t>> a_skins;
    for (size_t k = 0; k < a.models.size(); ++k) {
        const Real3dScene::Model& model = a.models[k];
        if (model.viewport >= a_known.size() || !a_known[model.viewport] || !built_each_frame(model)) continue;
        if (const uint64_t key = body_key(a, a_unview[model.viewport] * affine(model.matrix)); key != kNoKey)
            a_skins[key].push_back(k);
    }
    std::unordered_map<uint64_t, size_t> b_skin_count;
    auto same_layout = [&](const Real3dScene::Model& x, const Real3dScene::Model& y) {
        if (x.polygon_count != y.polygon_count) return false;
        for (uint32_t p = 0; p < x.polygon_count; ++p)
            if (a.polygons[x.first_polygon + p].vertex_count != b.polygons[y.first_polygon + p].vertex_count) return false;
        return true;
    };

    // A skin moves with its bone: drawn with its bone's turn, keyed by body and
    // bone, it goes through the skeleton as the limbs do. Not keyed (or its body
    // put down in one jump): as the limb drawn with the same turn (bit for bit),
    // the nearest (it sits a little off the limb's own point; any point fixed to
    // the bone moves as the limb does); none (its bone's own model left out at
    // a lower detail), the nearest model; none, as in the newer frame.
    // Its shape is blended too: paired with a's, each vertex goes part of the
    // way back to where a's had it in the skin's own frame (a skirt between
    // the waist and both thighs bends with the legs).
    for (size_t i : skins) {
        const Real3dScene::Model& skin = b.models[i];
        const Affine skin_place = d.views[skin.viewport].unview * affine(skin.matrix);
        const uint64_t key = body_key(b, skin_place);
        if (!placed_near(skin_place.t) && on_bone(d.plans[i], key)) ++s.skins;
        else {
            size_t limb = b.models.size();
            float nearest = kSkinReach;
            const Vec3 at = translation(skin.matrix);
            for (const bool same_turn : {true, false}) {
                for (size_t k = 0; k < b.models.size(); ++k) {
                    const Real3dScene::Model& other = b.models[k];
                    if (other.viewport != skin.viewport || built_each_frame(other) ||
                        (same_turn && !same_linear_bits(other.matrix, skin.matrix)))
                        continue;
                    if (const float dk = distance(translation(other.matrix), at); dk < nearest) {
                        nearest = dk;
                        limb = k;
                    }
                }
                if (limb != b.models.size()) {
                    if (!same_turn) ++s.skins_near;
                    break;
                }
            }
            if (limb == b.models.size()) {
                ++s.skins_alone;
                continue;
            }
            d.plans[i].kind = Data::Kind::follow;
            d.plans[i].follow = static_cast<uint32_t>(limb);
            ++s.skins;
        }
        // its shape in a
        if (key == kNoKey) continue;
        const size_t order = b_skin_count[key]++;
        const auto pair = a_skins.find(key_in_a(key, a_slot_of));
        if (pair == a_skins.end() || order >= pair->second.size()) continue;
        const Real3dScene::Model& before_skin = a.models[pair->second[order]];
        Mat3 la_inverse{};
        if (!same_layout(before_skin, skin) || !inverse(linear(before_skin.matrix), la_inverse)) continue;
        const Mat3 lb = linear(skin.matrix);
        const Vec3 ta = translation(before_skin.matrix), tb = translation(skin.matrix);
        std::vector<Vec3>& shape = d.plans[i].shape;
        for (uint32_t p = 0; p < skin.polygon_count; ++p) {
            const Real3dScene::Polygon &pa = a.polygons[before_skin.first_polygon + p], &pb = b.polygons[skin.first_polygon + p];
            for (uint32_t k = 0; k < pb.vertex_count; ++k) {
                const Real3dScene::Vertex &va = a.vertices[pa.first_vertex + k], &vb = b.vertices[pb.first_vertex + k];
                // a's vertex in the skin's own frame, put in b's skin frame
                const Vec3 local = m3::apply(la_inverse, {va.x - ta[0], va.y - ta[1], va.z - ta[2]});
                const Vec3 at_b = m3::apply(lb, local);
                shape.push_back({at_b[0] + tb[0] - vb.x, at_b[1] + tb[1] - vb.y, at_b[2] + tb[2] - vb.z});
            }
        }
        ++s.skins_shaped;
    }
    return true;
}

void CameraBlend::draw(float t, Real3dScene& out) const {
    const Data& d = *d_;
    out = *d.b;
    if (!d.blend || d.plans.size() != d.b->models.size()) return;
    const Real3dScene& b = *d.b;
    // The camera between, per viewport, and the move from b's eye space to it.
    std::vector<Affine> moves(b.viewports.size());
    // (and the eye between, its heading and b's: for the effects rebuilt as the game draws them)
    std::vector<Vec3> eyes(b.viewports.size());
    std::vector<float> headings(b.viewports.size()), headings_b(b.viewports.size());
    for (size_t v = 0; v < b.viewports.size(); ++v) {
        const Data::View& view = d.views[v];
        if (!view.moved) continue;
        const CameraParts &pa = view.pa, &pb = view.pb;
        Vec3 scale;
        for (size_t c = 0; c < 3; ++c) scale[c] = pa.parts.scale[c] + (pb.parts.scale[c] - pa.parts.scale[c]) * t;
        const Mat3 lt = linear_from(slerp(pa.parts.q, pb.parts.q, t), scale, pa.parts.mirrored);
        // The eye blended without the shake (it only lifts the eye), then the
        // newer frame's shake put back: blended, a shake that changes
        // direction every frame or two would be halved.
        const float ya = pa.eye[1] - view.shake_a, yb = pb.eye[1] - view.shake_b;
        const Vec3 eye{pa.eye[0] + (pb.eye[0] - pa.eye[0]) * t, ya + (yb - ya) * t + view.shake_b,
                       pa.eye[2] + (pb.eye[2] - pa.eye[2]) * t};
        const Vec3 le = m3::apply(lt, eye);
        eyes[v] = eye;
        headings[v] = camera_heading(lt);
        headings_b[v] = camera_heading(view.view.l);
        const Mat3 l = times(lt, view.lb_inverse);
        const Vec3 l_tb = m3::apply(l, view.tb);
        moves[v] = {l, {-le[0] - l_tb[0], -le[1] - l_tb[1], -le[2] - l_tb[2]}};
        // The sun, given in eye space, turned with the rest.
        Real3dScene::Viewport& vp = out.viewports[v];
        Vec3 sun = m3::apply(l, {vp.sun[0], vp.sun[1], vp.sun[2]});
        if (const float n = length(sun); n > 1e-12f)
            for (float& c : sun) c *= length({vp.sun[0], vp.sun[1], vp.sun[2]}) / n;
        vp.sun = {sun[0], sun[1], sun[2]};
    }
    // The skeletons between: each bone's turn and place relative to its
    // parent blended, then the chain built from the root.
    std::vector<std::array<Affine, Real3dScene::kGameBones>> bones_t(d.skeletons.size());
    for (size_t sk = 0; sk < d.skeletons.size(); ++sk) {
        const Data::Skeleton& skeleton = d.skeletons[sk];
        for (size_t k = 0; k < bones_t[sk].size(); ++k) {
            const Parts &pa = skeleton.pa[k], &pb = skeleton.pb[k];
            Vec3 scale;
            for (size_t c = 0; c < 3; ++c) scale[c] = pa.scale[c] + (pb.scale[c] - pa.scale[c]) * t;
            Affine local{linear_from(slerp(pa.q, pb.q, t), scale, pa.mirrored), {}};
            for (size_t c = 0; c < 3; ++c) local.t[c] = skeleton.ta[k][c] + (skeleton.tb[k][c] - skeleton.ta[k][c]) * t;
            const int parent = kBoneParent[k];
            bones_t[sk][k] = parent < 0 ? local : bones_t[sk][static_cast<size_t>(parent)] * local;
        }
    }
    // Each model's move: the camera's, after its own place blended.
    std::vector<Affine> model_moves(b.models.size());
    for (size_t i = 0; i < b.models.size(); ++i) {
        const Real3dScene::Model& model = b.models[i];
        if (model.viewport >= d.views.size() || !d.views[model.viewport].moved) continue;
        const Data::View& view = d.views[model.viewport];
        const Data::Plan& plan = d.plans[i];
        model_moves[i] = moves[model.viewport];
        if (plan.kind == Data::Kind::bone) { // eye b -> world b -> its bone's own frame (b's) -> the bone between -> picture
            const Data::Skeleton& skeleton = d.skeletons[plan.skeleton];
            model_moves[i] = model_moves[i] * view.view * bones_t[plan.skeleton][plan.bone] *
                             skeleton.b_inverse[plan.bone] * view.unview;
            continue;
        }
        if (plan.kind == Data::Kind::shift) { // eye b -> world b -> moved back part of the way -> eye b -> picture
            const float back = 1 - t;
            const Affine shift{Mat3{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}},
                               {plan.shift[0] * back, plan.shift[1] * back, plan.shift[2] * back}};
            model_moves[i] = model_moves[i] * view.view * shift * view.unview;
            continue;
        }
        if (plan.kind == Data::Kind::billboard && plan.effect_exact) {
            // As the game draws it then: its place between, pulled toward the eye between; turned about that point
            // by its heading's change less the camera's (its roll's change: each is the game's own rule); its own
            // offset from the point turning with it.
            Vec3 place;
            for (size_t c = 0; c < 3; ++c) place[c] = plan.ta[c] + (plan.tb[c] - plan.ta[c]) * t;
            const Vec3 at = (model_moves[i] * view.view *
                             Affine{Mat3{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}, pulled(place, plan.pull, eyes[model.viewport])})
                                .t;
            const float turn = -(angle_step(plan.heading_a, plan.heading_b) + plan.spin_step) * (1 - t) -
                               angle_step(headings_b[model.viewport], headings[model.viewport]);
            const Mat3 r = roll_about_view(turn);
            // (turned about the anchor less the piece's way back to a's offset, part of the way: the piece's own
            // offset blended too)
            const float back = 1 - t;
            const Vec3 ra = m3::apply(r, {plan.anchor_b[0] - plan.offset_back[0] * back, plan.anchor_b[1] - plan.offset_back[1] * back,
                                          plan.anchor_b[2] - plan.offset_back[2] * back});
            model_moves[i] = {r, {at[0] - ra[0], at[1] - ra[1], at[2] - ra[2]}};
            continue;
        }
        if (plan.kind == Data::Kind::billboard) { // its turn as in b (facing the eye); its place, moved there
            Vec3 world;
            for (size_t c = 0; c < 3; ++c) world[c] = plan.ta[c] + (plan.tb[c] - plan.ta[c]) * t;
            const Affine eye_b = view.view * Affine{Mat3{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}, world};
            const Vec3 to = (model_moves[i] * eye_b).t, from = translation(model.matrix);
            model_moves[i] = {Mat3{Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}},
                              {to[0] - from[0], to[1] - from[1], to[2] - from[2]}};
            continue;
        }
        if (plan.kind != Data::Kind::blend) continue;
        Vec3 scale;
        for (size_t c = 0; c < 3; ++c) scale[c] = plan.pa.scale[c] + (plan.pb.scale[c] - plan.pa.scale[c]) * t;
        Affine between{linear_from(slerp(plan.pa.q, plan.pb.q, t), scale, plan.pa.mirrored), {}};
        for (size_t k = 0; k < 3; ++k) between.t[k] = plan.ta[k] + (plan.tb[k] - plan.ta[k]) * t;
        // eye b -> world b (undo the camera) -> the model's own -> its place between -> eye b -> picture
        model_moves[i] = model_moves[i] * view.view * between * plan.place_inverse * view.unview;
    }
    for (size_t i = 0; i < b.models.size(); ++i) {
        const Real3dScene::Model& model = b.models[i];
        if (model.viewport >= d.views.size() || !d.views[model.viewport].moved) continue;
        const Data::Plan& plan = d.plans[i];
        const Affine& move = model_moves[plan.kind == Data::Kind::follow ? plan.follow : i];
        const Mat3& l = move.l;
        const Vec3& tr = move.t;
        const float back = 1 - t; // a skin's shape: part of the way back to a's
        size_t shaped = 0;
        for (uint32_t p = model.first_polygon; p < model.first_polygon + model.polygon_count; ++p) {
            const Real3dScene::Polygon& poly = out.polygons[p];
            for (uint32_t k = 0; k < poly.vertex_count; ++k) {
                Real3dScene::Vertex& vx = out.vertices[poly.first_vertex + k];
                if (shaped < plan.shape.size()) {
                    const Vec3& to_a = plan.shape[shaped++];
                    vx.x += to_a[0] * back;
                    vx.y += to_a[1] * back;
                    vx.z += to_a[2] * back;
                }
                const Vec3 pos = m3::apply(l, {vx.x, vx.y, vx.z});
                Vec3 n = m3::apply(l, {vx.nx, vx.ny, vx.nz});
                // normals kept their length (the move turns them; any scale isn't theirs)
                const float was = std::sqrt(vx.nx * vx.nx + vx.ny * vx.ny + vx.nz * vx.nz), now = length(n);
                if (now > 1e-12f)
                    for (float& c : n) c *= was / now;
                vx.x = pos[0] + tr[0];
                vx.y = pos[1] + tr[1];
                vx.z = pos[2] + tr[2];
                vx.nx = n[0];
                vx.ny = n[1];
                vx.nz = n[2];
            }
        }
    }
}

bool interpolate_camera(const Real3dScene& a, const Real3dScene& b, float t, Real3dScene& out, CameraInterpStats* stats,
                        bool models) {
    CameraBlend blend;
    const bool ok = blend.prepare(a, b, models);
    blend.draw(t, out);
    if (stats) *stats = blend.stats();
    return ok;
}

} // namespace m3

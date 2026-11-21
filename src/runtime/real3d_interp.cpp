#include "runtime/real3d_interp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

} // namespace

bool interpolate_camera(const Real3dScene& a, const Real3dScene& b, float t, Real3dScene& out, CameraInterpStats* stats) {
    out = b;
    CameraInterpStats s;
    auto done = [&](CameraInterpStats::Cut cut) {
        s.cut = cut;
        if (stats) *stats = s;
        return cut == CameraInterpStats::Cut::none;
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

    // Each of b's viewports with the game's camera, moved from it to the
    // camera between: by C = (between) x (b's)^-1, the camera's in a taken
    // from a viewport of a at the same address with the game's camera, else
    // the first with it.
    std::vector<size_t> a_cameras;
    for (size_t v = 0; v < a.viewports.size(); ++v)
        if (is_game_camera(a.viewports[v], ca)) a_cameras.push_back(v);
    if (a_cameras.empty()) return done(CameraInterpStats::Cut::no_viewport);
    std::vector<std::pair<Mat3, Vec3>> moves(b.viewports.size()); // per viewport: C's linear part and translation
    std::vector<bool> moved(b.viewports.size(), false);
    for (size_t v = 0; v < b.viewports.size(); ++v) {
        if (!is_game_camera(b.viewports[v], cb)) continue;
        size_t from = a_cameras.front();
        if (v < b.viewport_info.size())
            for (size_t k : a_cameras)
                if (k < a.viewport_info.size() && a.viewport_info[k].addr == b.viewport_info[v].addr) {
                    from = k;
                    break;
                }
        const Mat4 &va = a.viewports[from].camera, &vb = b.viewports[v].camera;
        CameraParts pa{}, pb{};
        Mat3 lb_inverse{};
        if (!camera_parts(va, pa) || !camera_parts(vb, pb) || pa.parts.mirrored != pb.parts.mirrored ||
            !inverse(linear(vb), lb_inverse))
            continue;
        const auto& qa = pa.parts.q;
        const auto& qb = pb.parts.q;
        const float dot = std::fabs(qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]);
        const float turn = 2.0f * std::acos(std::min(dot, 1.0f)) * 57.29578f;
        if (s.viewports == 0) s.turn = turn;
        if (turn > kCameraTurnJump) return done(CameraInterpStats::Cut::turn);
        Mat3 lt = rotation(slerp(qa, qb, t));
        if (pa.parts.mirrored)
            for (float& c : lt[0]) c = -c;
        for (size_t c = 0; c < 3; ++c)
            for (float& x : lt[c]) x *= pa.parts.scale[c] + (pb.parts.scale[c] - pa.parts.scale[c]) * t;
        // The eye blended without the shake (it only lifts the eye), then the
        // newer frame's shake put back: blended, a shake that changes
        // direction every frame or two would be halved.
        const float ya = pa.eye[1] - ca.shake, yb = pb.eye[1] - cb.shake;
        const Vec3 eye{pa.eye[0] + (pb.eye[0] - pa.eye[0]) * t, ya + (yb - ya) * t + cb.shake,
                       pa.eye[2] + (pb.eye[2] - pa.eye[2]) * t};
        const Vec3 le = m3::apply(lt, eye);
        const Vec3 tt{-le[0], -le[1], -le[2]}; // the camera between: L_t, -L_t eye
        const Mat3 l = times(lt, lb_inverse);
        const Vec3 l_tb = m3::apply(l, translation(vb));
        moves[v] = {l, {tt[0] - l_tb[0], tt[1] - l_tb[1], tt[2] - l_tb[2]}};
        moved[v] = true;
        ++s.viewports;
        // The sun, given in eye space, turned with the rest.
        Real3dScene::Viewport& vp = out.viewports[v];
        Vec3 sun = m3::apply(l, {vp.sun[0], vp.sun[1], vp.sun[2]});
        if (const float n = length(sun); n > 1e-12f)
            for (float& c : sun) c *= length({vp.sun[0], vp.sun[1], vp.sun[2]}) / n;
        vp.sun = {sun[0], sun[1], sun[2]};
    }
    if (s.viewports == 0) return done(CameraInterpStats::Cut::no_viewport);

    for (const Real3dScene::Model& model : b.models) {
        if (model.viewport >= moved.size() || !moved[model.viewport]) continue;
        ++s.models;
        const auto& [l, tr] = moves[model.viewport];
        for (uint32_t p = model.first_polygon; p < model.first_polygon + model.polygon_count; ++p) {
            const Real3dScene::Polygon& poly = out.polygons[p];
            for (uint32_t k = 0; k < poly.vertex_count; ++k) {
                Real3dScene::Vertex& vx = out.vertices[poly.first_vertex + k];
                const Vec3 pos = m3::apply(l, {vx.x, vx.y, vx.z}), n = m3::apply(l, {vx.nx, vx.ny, vx.nz});
                vx.x = pos[0] + tr[0];
                vx.y = pos[1] + tr[1];
                vx.z = pos[2] + tr[2];
                vx.nx = n[0];
                vx.ny = n[1];
                vx.nz = n[2];
            }
        }
    }
    return done(CameraInterpStats::Cut::none);
}

} // namespace m3

#include "games/spikeofe_camera.hpp"

#include <bit>
#include <cmath>

namespace m3::games {

namespace {

uint32_t word(std::span<const uint8_t> ram, uint32_t addr) {
    if (addr + 4 > ram.size()) return 0;
    return uint32_t{ram[addr]} << 24 | uint32_t{ram[addr + 1]} << 16 | uint32_t{ram[addr + 2]} << 8 | ram[addr + 3];
}

} // namespace

void spikeout_camera(std::span<const uint8_t> ram, Real3dScene::GameCamera& out) {
    out = {};
    if (kCameraWork + kCameraView + 48 > ram.size()) return;
    auto f = [&](uint32_t offset) { return std::bit_cast<float>(word(ram, kCameraWork + offset)); };
    for (uint32_t i = 0; i < 12; ++i) out.view[i] = word(ram, kCameraWork + kCameraView + 4 * i);
    for (uint32_t i = 0; i < 3; ++i) {
        out.eye[i] = f(kCameraEye + 4 * i);
        out.look[i] = f(kCameraLook + 4 * i);
    }
    // The shake only lifts the eye (and the look-at) by the same amount; the
    // view is made from the shaken eye with the unshaken angles.
    out.shake = f(kCameraEyeShaken + 4) - out.eye[1];
    if (!std::isfinite(out.shake)) out.shake = 0;
    out.driver = ram[kCameraWork + kCameraDriver];
    out.shot_timer = static_cast<uint16_t>(ram[kCameraWork + kCameraShotTimer] << 8 | ram[kCameraWork + kCameraShotTimer + 1]);
    out.shot = ram[kCameraWork + kCameraShot];
    out.known = true;
}

void SpikeoutCamera::read(std::span<const uint8_t> ram, Real3dScene& scene) {
    Real3dScene::GameCamera now;
    spikeout_camera(ram, now);
    const bool camera_changed = now != last_;
    if (camera_changed) {
        before_ = last_;
        last_ = now;
    }
    scene.game_camera = last_;
    scene.game_camera_before = before_;

    // The bodies, and which were put down in one jump since the last read.
    std::array<Body, kPlayerSlots + kEnemySlots> bodies{};
    for (uint32_t s = 0; s < bodies.size(); ++s) {
        const uint32_t base = s < kPlayerSlots ? kPlayers + kBodyStride * s : kEnemies + kBodyStride * (s - kPlayerSlots);
        if (base + kBodyRespawn >= ram.size()) return;
        Body& body = bodies[s];
        body.word0 = word(ram, base);
        for (uint32_t k = 0; k < 3; ++k) body.place[k] = std::bit_cast<float>(word(ram, base + kBodyPlace + 4 * k));
        body.respawn = ram[base + kBodyRespawn];
    }
    if (bodies != bodies_ || !bodies_read_) {
        placed_before_ = std::move(placed_now_);
        placed_now_.clear();
        if (bodies_read_)
            for (size_t s = 0; s < bodies.size(); ++s) {
                const Body &was = bodies_[s], &is = bodies[s];
                if (is.word0 == 0) continue;
                const float dx = is.place[0] - was.place[0], dy = is.place[1] - was.place[1], dz = is.place[2] - was.place[2];
                // made (the record was empty), put back on the ground, or moved further than anything walks
                // (a made body's lift the frame after, 2 units; a snap back)
                const bool placed = was.word0 == 0 || (is.respawn == kRespawned && was.respawn != kRespawned) ||
                                    !(dx * dx + dy * dy + dz * dz <= kBodyJump * kBodyJump);
                if (!placed) continue;
                placed_now_.push_back(is.place);
                if (was.word0 != 0) placed_now_.push_back(was.place);
            }
        bodies_ = bodies;
        bodies_read_ = true;
    }
    scene.placed = placed_now_;
    scene.placed.insert(scene.placed.end(), placed_before_.begin(), placed_before_.end());
}

} // namespace m3::games

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

} // namespace m3::games

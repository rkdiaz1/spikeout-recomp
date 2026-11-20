#include "games/spikeofe_camera.hpp"

#include <bit>

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
    for (uint32_t i = 0; i < 12; ++i) out.view[i] = word(ram, kCameraWork + kCameraView + 4 * i);
    // The eye from the matrix itself (what was drawn): -R^T t, R column-major.
    float m[12];
    for (int i = 0; i < 12; ++i) m[i] = std::bit_cast<float>(out.view[static_cast<size_t>(i)]);
    for (int r = 0; r < 3; ++r) // (R^T t)[r] = the r-th column of R dotted with t
        out.eye[static_cast<size_t>(r)] = -(m[r * 3] * m[9] + m[r * 3 + 1] * m[10] + m[r * 3 + 2] * m[11]);
    for (uint32_t i = 0; i < 3; ++i)
        out.look[i] = std::bit_cast<float>(word(ram, kCameraWork + kCameraLook + 4 * i));
    out.driver = ram[kCameraWork + kCameraDriver];
    out.shaking = ram[kCameraWork + kCameraShakeLeft] != 0;
    out.known = true;
}

} // namespace m3::games

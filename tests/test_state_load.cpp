// Save states from a file or a network peer: a field that would index outside a device's memory or tables is
// refused when loaded (the State error path, std::runtime_error), and a device's own state still loads.
//
// Each case changes one field of a saved state, found by its place from the end of the device's fields (the
// order its state() walks them); a check of the state's size or of a known field first keeps that place honest.
#include "test.hpp"

#include "runtime/crypt315_5881.hpp"
#include "runtime/dsb2.hpp"
#include "runtime/eeprom.hpp"
#include "runtime/jtag.hpp"
#include "runtime/real3d.hpp"
#include "runtime/real3d_scene.hpp"
#include "runtime/scsp.hpp"
#include "runtime/state.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace m3;

namespace {

template <class Device> std::vector<uint8_t> saved(Device& device) {
    State s;
    device.state(s);
    return std::move(s.data());
}

template <class T> T field(const std::vector<uint8_t>& bytes, std::size_t from_end) {
    T v;
    std::memcpy(&v, bytes.data() + bytes.size() - from_end, sizeof v);
    return v;
}

// Whether `device` loads `bytes` (false: refused as a bad state).
template <class Device> bool loads(Device& device, std::vector<uint8_t> bytes) {
    State in(std::move(bytes));
    try {
        device.state(in);
    } catch (const std::runtime_error&) {
        return false;
    }
    return in.done();
}

// Whether `device` loads `bytes` with `value` put at `from_end` bytes before their end.
template <class Device, class T> bool loads_with(Device& device, std::vector<uint8_t> bytes, std::size_t from_end, T value) {
    std::memcpy(bytes.data() + bytes.size() - from_end, &value, sizeof value);
    return loads(device, std::move(bytes));
}

// `bytes` loaded into `device` save the same bytes again.
template <class Device> bool round_trips(Device& device, const std::vector<uint8_t>& bytes) {
    return loads(device, bytes) && saved(device) == bytes;
}

uint32_t bswap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }

} // namespace

TEST_CASE("state load: the EEPROM's bit counts") {
    Eeprom93C46 eeprom;
    const std::vector<uint8_t> bytes = saved(eeprom);
    CHECK_EQ(bytes.size(), std::size_t{164});
    // From the end: busy_until_ 8, addr_ 1, out_bits_ 4, out_ 2, in_bits_ 4.
    constexpr std::size_t kInBits = 19, kOutBits = 13;
    CHECK(loads_with(eeprom, bytes, kInBits, int{25}));
    CHECK(!loads_with(eeprom, bytes, kInBits, int{26}));
    CHECK(!loads_with(eeprom, bytes, kInBits, int{-1}));
    CHECK(loads_with(eeprom, bytes, kOutBits, int{15}));
    CHECK(!loads_with(eeprom, bytes, kOutBits, int{16}));
    CHECK(round_trips(eeprom, bytes));
}

TEST_CASE("state load: the SCSP's envelopes, LFO tables, DSP steps and ring buffer") {
    std::vector<uint8_t> ram(Scsp::kRamSize, 0);
    Scsp scsp(ram, true);
    constexpr uint32_t kLast = 31 * 0x20; // the last slot: its fields end nearest the DSP's
    scsp.write16(kLast + 0x12, 0x0001);   // ALFOS 1 (saw)
    scsp.write16(kLast + 0x08, 0x001F);   // AR 31
    scsp.write16(kLast + 0x00, 0x1800);   // KYONEX, KYONB
    CHECK_EQ(scsp.active_slots(), 1);
    const std::vector<uint8_t> bytes = saved(scsp);
    // "scsp1", the registers, 32 slots of 105 bytes, the DSP's 1,973, ring_ 128, bufptr_, then 278 more.
    CHECK_EQ(bytes.size(), std::size_t{6 + 48 + 32 * 105 + 1973 + 128 + 1 + 278});
    constexpr std::size_t kBufptr = 278 + 1, kLastStep = kBufptr + 128 + 4, kAfterSlots = kLastStep - 4 + 1973;
    // The last slot's last fields, from its end: LFO rows (plfo table, scale, alfo table, scale) 16, LFO phases and
    // steps 12, below_dl, hold, dl, rr, d2r, d1r, ar, state, volume.
    constexpr std::size_t kAlfoScale = kAfterSlots + 4, kAlfoTable = kAfterSlots + 8;
    constexpr std::size_t kAr = kAfterSlots + 16 + 12 + 2 + 4 * 4 + 4, kVolume = kAr + 1 + 4;

    CHECK(loads_with(scsp, bytes, kBufptr, uint8_t{63}));
    CHECK(!loads_with(scsp, bytes, kBufptr, uint8_t{64}));
    CHECK(loads_with(scsp, bytes, kLastStep, int{128}));
    CHECK(!loads_with(scsp, bytes, kLastStep, int{129}));
    CHECK(!loads_with(scsp, bytes, kLastStep, int{-1}));
    CHECK(loads_with(scsp, bytes, kVolume, int{0x3FF << 16}));
    CHECK(!loads_with(scsp, bytes, kVolume, int{0x400 << 16}));
    CHECK(!loads_with(scsp, bytes, kVolume, int{-1}));
    CHECK(loads_with(scsp, bytes, kAr, int{0}));
    CHECK(!loads_with(scsp, bytes, kAr, int{12345}));
    CHECK(!loads_with(scsp, bytes, kAlfoScale, int32_t{8})); // 8 scales
    CHECK(!loads_with(scsp, bytes, kAlfoScale, int32_t{-2}));
    CHECK(!loads_with(scsp, bytes, kAlfoTable, int32_t{-1})); // its slot plays with the LFO on: a table needed
    CHECK(round_trips(scsp, bytes));
}

TEST_CASE("state load: the music board's place in the MPEG ROM, decoded samples and decoder") {
    std::vector<uint8_t> program(0x20000, 0), mpeg(16, 0);
    Dsb2 dsb(program, mpeg);
    const std::vector<uint8_t> bytes = saved(dsb);
    // From the end: irqs_taken 64, commands, tracks_started, samples_ 8 each, frac_ 4, next_, prev_ 8 each,
    // stream_hz_, stream_layer_, pcm_channels_, pcm_pos_, pcm_count_ 4 each, then pcm_ (its length, 2,304 samples).
    constexpr std::size_t kChannels = 120, kPcmPos = 124, kPcmCount = 128, kPcmLength = kPcmCount + 2304 * 2 + 8;
    constexpr std::size_t kPos = kPcmLength + 4, kPlayEnd = kPos + 4, kPlayStart = kPlayEnd + 4;
    // Before play_start_: loop_, playing_, volume_, stereo_, end_, start_, state_, control_, next_timer_, next_rx_,
    // timer_irq_, rx_ready_, rx_, line_ (empty: its length), then the decoder's reserv_buf, header, free_format_bytes, reserv.
    constexpr std::size_t kLine = kPlayStart + 45, kFreeFormat = kLine + 511 + 4 + 4, kReserv = kFreeFormat + 4;
    CHECK_EQ(field<uint64_t>(bytes, kPcmLength), uint64_t{2304});
    CHECK_EQ(field<uint64_t>(bytes, kLine), uint64_t{0});

    CHECK(loads_with(dsb, bytes, kPos, uint32_t{16}));
    CHECK(!loads_with(dsb, bytes, kPos, uint32_t{17}));
    CHECK(!loads_with(dsb, bytes, kPlayEnd, uint32_t{17}));
    CHECK(!loads_with(dsb, bytes, kPlayStart, uint32_t{1})); // after play_end_ (0)
    CHECK(!loads_with(dsb, bytes, kChannels, int{3}));
    CHECK(!loads_with(dsb, bytes, kChannels, int{0}));
    CHECK(!loads_with(dsb, bytes, kPcmPos, int{1})); // past pcm_count_ (0)
    CHECK(!loads_with(dsb, bytes, kPcmCount, int{1153})); // 2 channels: past pcm_'s 2,304
    CHECK(!loads_with(dsb, bytes, kPcmCount, int{-1}));
    CHECK(!loads_with(dsb, bytes, kReserv, int{512}));
    CHECK(!loads_with(dsb, bytes, kFreeFormat, int{-1}));
    {
        std::vector<uint8_t> shorter = bytes; // pcm_ a sample short
        shorter.erase(shorter.end() - static_cast<std::ptrdiff_t>(kPcmCount + 2), shorter.end() - static_cast<std::ptrdiff_t>(kPcmCount));
        const uint64_t length = 2303;
        std::memcpy(shorter.data() + shorter.size() - (kPcmLength - 2), &length, sizeof length);
        CHECK(!loads(dsb, shorter));
    }
    CHECK(round_trips(dsb, bytes));
}

TEST_CASE("state load: the 3D's update buffers") {
    Real3d r;
    const std::vector<uint8_t> bytes = saved(r);
    CHECK_EQ(bytes.size(), std::size_t{7 + 8 + 4 * 0x100000 + 8 + 4 * 0x40000 + 8 + 4 * 0x100000 + 8 + 2 * 2048 * 2048 + 18 + 4 + 16 + 8 + 8});
    // From the end: textures_uploaded_ 8, fifo_ (empty: its length) 8, config_ 16, four flags; then the polygon
    // buffer and the high culling RAM's, each cursor, block, used.
    constexpr std::size_t kHighCursor = 8 + 8 + 16 + 4 + 9 + 9, kHighBlock = kHighCursor - 4, kHighUsed = kHighBlock - 4;
    auto with_buffer = [&](uint32_t cursor, uint32_t block) {
        std::vector<uint8_t> b = bytes;
        std::memcpy(b.data() + b.size() - kHighCursor, &cursor, 4);
        std::memcpy(b.data() + b.size() - kHighBlock, &block, 4);
        b[b.size() - kHighUsed] = 1;
        return b;
    };
    CHECK(loads(r, with_buffer(3, 0)));
    CHECK(loads(r, with_buffer(0x100000, UINT32_MAX)));
    CHECK(!loads(r, with_buffer(0x100001, UINT32_MAX)));
    CHECK(!loads(r, with_buffer(0xFFFFFFFE, UINT32_MAX))); // (cursor + 3 would wrap)
    CHECK(!loads(r, with_buffer(3, 1)));                   // the block's data past the cursor
    CHECK(!loads(r, with_buffer(2, 0)));
    CHECK(!loads(r, with_buffer(0, UINT32_MAX - 1)));
    CHECK(round_trips(r, bytes));
}

TEST_CASE("real3d: a buffered block whose last word the game put before its first ends the update") {
    Real3d r;
    r.write_config(0, bswap32(0x100)); // high culling RAM's buffer: low culling RAM 0-0xFF
    r.write_config(8, bswap32(0x200));
    r.ping_pong_flipped();
    r.write_culling_high(0, bswap32(0x1234)); // buffered: block 0 (first 0, last 0, 0x1234)
    r.write_culling_low(4, bswap32(0xFFFFFFFD)); // its last word now first - 3: a step of 0 words
    r.begin_vblank(0);
    CHECK_EQ(r.culling(0x800000), 0x1234u);
}

namespace {

uint32_t fbits(float f) { return std::bit_cast<uint32_t>(f); }

// One viewport over the whole screen whose root is the culling node at `root`, nodes put by `cull`.
template <class Cull> void viewport(Cull cull, uint32_t root, uint32_t next = 0x01000000) {
    cull(0x800000 + 0x01, next);
    cull(0x800000 + 0x02, root);
    for (uint32_t w : {0x8u, 0x9u, 0xAu, 0xBu}) cull(0x800000 + w, fbits(0.5f));
    cull(0x800000 + 0x14, (384u * 4) << 16 | 496u * 4);
    cull(0x800000 + 0x16, 0x1000); // matrix base
    const float identity[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (uint32_t i = 0; i < 12; ++i) cull(0x1000 + i, fbits(identity[i]));
}

// A culling node: translation only, no LOD table, the child pointer and sibling given.
template <class Cull> void node(Cull cull, uint32_t addr, uint32_t child, uint32_t sibling) {
    cull(addr + 0, 0x11);
    cull(addr + 3, 0x7Fu << 12);
    cull(addr + 7, child);
    cull(addr + 8, sibling);
}

} // namespace

TEST_CASE("real3d scene: siblings drawn last first, a looping chain and a viewport past memory's end stop") {
    Real3d r;
    auto cull = [&](uint32_t word_addr, uint32_t value) {
        if (word_addr >= 0x800000) r.write_culling_high((word_addr - 0x800000) * 4, bswap32(value));
        else r.write_culling_low(word_addr * 4, bswap32(value));
    };
    viewport(cull, 0x800100);
    node(cull, 0x800100, 0x01000200, 0x800110); // model 0x200, then its sibling
    node(cull, 0x800110, 0x01000300, 0);         // model 0x300
    Real3dScene scene;
    Real3dSceneBuilder().build(r, scene);
    CHECK_EQ(scene.stats.nodes, 2u);
    CHECK_EQ(scene.models.size(), 2u);
    if (scene.models.size() == 2) {
        CHECK_EQ(scene.models[0].address, 0x300u); // the sibling's first, as Supermodel
        CHECK_EQ(scene.models[1].address, 0x200u);
    }

    node(cull, 0x800110, 0, 0x800110); // its own sibling: walked until the visits run out, not the stack
    Real3dSceneBuilder().build(r, scene);
    CHECK(scene.stats.nodes > 1000u);
    CHECK_EQ(scene.models.size(), 1u);

    viewport(cull, 0x800100, 0x83FFF0); // the next viewport's words past high culling RAM's end: not read
    Real3dSceneBuilder().build(r, scene);
    CHECK_EQ(scene.viewports.size(), 1u);
}

TEST_CASE("state load: the decryption chip's buffer places and bit numbers") {
    Crypt315_5881 crypt;
    const std::vector<uint8_t> bytes = saved(crypt);
    // From the end: done_compression_ 1, block_numlines_, block_pos_, block_size_ 4 each, buffer2a_ 2, buffer2_ 2,
    // buffer_bit2_, buffer_bit_, line_buffer_size_, line_buffer_pos_, buffer_pos_ 4 each.
    constexpr std::size_t kBit2 = 21, kBit = 25, kLineSize = 29, kLinePos = 33, kPos = 37;
    CHECK_EQ(field<int>(bytes, kBit2), 15);
    CHECK(loads_with(crypt, bytes, kPos, int{2}));
    CHECK(!loads_with(crypt, bytes, kPos, int{3}));
    CHECK(!loads_with(crypt, bytes, kPos, int{-2}));
    CHECK(loads_with(crypt, bytes, kLineSize, int{512}));
    CHECK(!loads_with(crypt, bytes, kLineSize, int{513}));
    CHECK(!loads_with(crypt, bytes, kLinePos, int{-1}));
    CHECK(!loads_with(crypt, bytes, kBit, int{8}));
    CHECK(!loads_with(crypt, bytes, kBit2, int{16}));
    CHECK(round_trips(crypt, bytes));
}

TEST_CASE("state load: the JTAG chain's state and register lengths") {
    Jtag jtag;
    const std::vector<uint8_t> bytes = saved(jtag);
    // From the end: mem_config_ 12, modewords_ 20, last_tck_, tdo_, state_ 1 each; then the last device's
    // instruction 1 and length 4.
    constexpr std::size_t kState = 35, kLength = 40;
    CHECK_EQ(field<uint8_t>(bytes, kState), uint8_t{0});
    CHECK(loads_with(jtag, bytes, kState, uint8_t{15}));
    CHECK(!loads_with(jtag, bytes, kState, uint8_t{16}));
    CHECK(loads_with(jtag, bytes, kLength, uint32_t{64}));
    CHECK(!loads_with(jtag, bytes, kLength, uint32_t{65}));
    CHECK(!loads_with(jtag, bytes, kLength, uint32_t{0}));
    CHECK(round_trips(jtag, bytes));
}

// Fuzzing (roadmap, safe loading): each device's state with random bytes changed, loaded, and run a little if it
// loads; nothing may read or write outside memory (run under AddressSanitizer and UBSan to see it: Linux,
// -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"). A fixed seed: the same cases each run; SPIKEOUT_FUZZ=N runs N
// rounds a device (default 300).
namespace {

struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    uint64_t next() {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        return s;
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

std::size_t fuzz_rounds() {
    const char* env = std::getenv("SPIKEOUT_FUZZ");
    return env ? static_cast<std::size_t>(std::strtoull(env, nullptr, 10)) : 300;
}

// `bytes` with a few bytes changed: most in the last 4 KB (the small fields, after the big buffers), some anywhere.
std::vector<uint8_t> mutated(const std::vector<uint8_t>& bytes, Rng& rng) {
    std::vector<uint8_t> b = bytes;
    const std::size_t n = 1 + rng.below(4);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t tail = std::min<std::size_t>(b.size(), 4096);
        const std::size_t at = rng.below(4) ? b.size() - 1 - rng.below(tail) : rng.below(b.size());
        switch (rng.below(3)) {
        case 0: b[at] = static_cast<uint8_t>(rng.next()); break;
        case 1: b[at] ^= static_cast<uint8_t>(1u << rng.below(8)); break;
        default: b[at] = rng.below(2) ? 0xFF : 0x80; break;
        }
    }
    return b;
}

// Each round: a mutated state into `device`; `run` if it loads. Then the good state loads again (a refused one
// leaves the device usable). Returns how many loaded (some must, or the mutations never reach the checks' fields).
template <class Device, class Run> std::size_t fuzz(Device& device, const std::vector<uint8_t>& bytes, Run run, uint64_t seed) {
    Rng rng{seed};
    std::size_t loaded = 0;
    for (std::size_t round = 0, rounds = fuzz_rounds(); round < rounds; ++round) {
        static const bool trace = std::getenv("SPIKEOUT_FUZZ_TRACE") != nullptr; // (each round as it starts: one that hangs found)
        std::vector<uint8_t> m = mutated(bytes, rng);
        if (trace) {
            std::printf("round %zu:", round);
            for (std::size_t i = 0; i < m.size(); ++i)
                if (m[i] != bytes[i]) std::printf(" %zu from the end: %02x->%02x", bytes.size() - i, bytes[i], m[i]);
            std::printf("\n"), std::fflush(stdout);
        }
        if (loads(device, std::move(m))) ++loaded, run(device);
        if (!loads(device, bytes)) return 0;
    }
    return loaded;
}

} // namespace

TEST_CASE("state load fuzz: SCSP") {
    std::vector<uint8_t> ram(Scsp::kRamSize, 0);
    Scsp scsp(ram, true);
    for (uint32_t slot = 0; slot < 4; ++slot) { // (a few playing, LFOs on: their tables in the state)
        const uint32_t at = slot * 0x20;
        scsp.write16(at + 0x12, static_cast<uint16_t>(0x00E1 + slot));
        scsp.write16(at + 0x08, 0x001F);
        scsp.write16(at + 0x00, 0x1800);
    }
    const std::vector<uint8_t> bytes = saved(scsp);
    CHECK(fuzz(scsp, bytes, [](Scsp& s) {
        int32_t l = 0, r = 0;
        for (int i = 0; i < 64; ++i) s.sample(l, r), s.tick_timers();
    }, 1) > 0);
}

TEST_CASE("state load fuzz: the music board") {
    std::vector<uint8_t> program(0x20000, 0), mpeg(0x1000, 0);
    Dsb2 dsb(program, mpeg);
    const std::vector<uint8_t> bytes = saved(dsb);
    CHECK(fuzz(dsb, bytes, [](Dsb2& d) {
        int16_t out[64 * 2];
        d.render(out, 64);
    }, 2) > 0);
}

TEST_CASE("state load fuzz: EEPROM, JTAG and the decryption chip") {
    Eeprom93C46 eeprom;
    CHECK(fuzz(eeprom, saved(eeprom), [](Eeprom93C46& e) {
        for (uint64_t t = 0; t < 64; ++t) e.write_pins(true, (t & 1) != 0, (t & 2) != 0, t * 100);
    }, 3) > 0);
    Jtag jtag;
    CHECK(fuzz(jtag, saved(jtag), [](Jtag& j) {
        for (int t = 0; t < 256; ++t) j.write((t & 1) != 0, (t & 6) == 2, (t & 8) != 0, false);
    }, 4) > 0);
    Crypt315_5881 crypt;
    crypt.set_key(0x12345678);
    crypt.set_reader([](uint32_t a) { return static_cast<uint16_t>(a * 0x9E37u); });
    CHECK(fuzz(crypt, saved(crypt), [](Crypt315_5881& c) {
        for (int i = 0; i < 2048; ++i) c.decrypt();
    }, 5) > 0);
}

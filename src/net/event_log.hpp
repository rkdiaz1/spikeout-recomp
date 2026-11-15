// Network play's timing, kept for problem reports (the owner, 2026-11-12: "a way to reproduce those problems from the
// reports, otherwise a fix won't be sure"). A game's recording (tools/net_record.hpp) has every player's buttons, frame
// by frame, but not when each came over the network; a room standing still is about when. So each machine keeps its
// last few minutes of what came and went, with the time: each datagram of inputs (from whom, whose, which frames),
// each frame's own inputs made here, and the session's turning points (a cabinet taken, a player dropped). A problem
// bundle and Report a problem take it in (net-events.txt); gamerun --net-replay FILE --net-arrivals net-events.txt holds
// each frame's inputs until the time they came here, to run the game again as this machine had it.
//
// Global (one ring for the program, whatever session writes): read from any thread at any time, a session gone or not.
// Cheap: a lock and a few bytes per event; ~200 events a second in a game of four.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net::events {

enum class Kind : uint8_t {
    inputs_in = 1, // from `from`: seat `seat`'s inputs, frames a .. a + b - 1 (as sent: some may be had already)
    local = 2,     // this machine's own inputs made, for frame a (seat `seat`)
    taken = 3,     // seat `seat`'s cabinet a player's from frame a
    idle = 4,      // seat `seat` dropped: its cabinet idle from frame a
    started = 5,   // the game began here: seats a, this machine's seat `seat` (0xFF: a spectator), from frame b
    note = 6,      // a turning point said in words (the session's log line): `text`
    delay = 7,     // this machine's input delay a from its frame b on (the game's start, Auto's changes)
    msg = 8,       // a session message from `from`: its type a, its size b, its bytes in `text` (hex; the first 256 only)
    sent = 9,      // a session message to `from` (the seat sent to), as msg
    sync = 10,     // time sync, once a second: ahead a frames, behind b, `text` its frames slowed / sooner / unpaced since
};

struct Event {
    double t = 0; // seconds since the first event of the run
    Kind kind{};
    uint8_t from = 0xFF, seat = 0xFF;
    uint64_t a = 0, b = 0;
    std::string text;
    uint64_t seq = 0; // (its number in the run: the game's file, record_to, takes them in order)
};

namespace detail {
struct Ring {
    std::mutex lock;
    std::vector<Event> events; // (a ring: kKept, the oldest overwritten)
    std::size_t next = 0;
    bool wrapped = false;
    uint64_t count = 0; // (events added in all: the newest's seq)
    std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
};
inline Ring& ring() {
    static Ring* r = new Ring; // (never destroyed: sessions may still write at exit)
    return *r;
}
constexpr std::size_t kKept = 60000; // (about 5 minutes of a four-player game)

// The events numbered after `after`, oldest first (as many as the ring still holds).
inline std::vector<Event> since(uint64_t after) {
    Ring& r = ring();
    std::lock_guard guard(r.lock);
    std::vector<Event> out;
    const uint64_t have = std::min<uint64_t>(r.count, r.events.size());
    const uint64_t first = r.count - have; // (the oldest's seq - 1)
    for (uint64_t s = std::max(after, first); s < r.count; ++s) {
        const std::size_t at = static_cast<std::size_t>((r.next + r.events.size() - (r.count - s)) % r.events.size());
        out.push_back(r.events[at]);
    }
    return out;
}
} // namespace detail

inline void add(Kind kind, uint8_t from, uint8_t seat, uint64_t a, uint64_t b = 0, std::string text = {}) {
    detail::Ring& r = detail::ring();
    Event e{std::chrono::duration<double>(std::chrono::steady_clock::now() - r.origin).count(), kind, from, seat, a, b, std::move(text), 0};
    std::lock_guard guard(r.lock);
    e.seq = ++r.count;
    if (r.events.size() < detail::kKept) r.events.push_back(std::move(e));
    else r.events[r.next] = std::move(e), r.wrapped = true;
    r.next = (r.next + 1) % detail::kKept;
}

// A message's bytes, for Kind::msg: hex, the first `most` only.
inline std::string hex(const uint8_t* data, std::size_t size, std::size_t most = 256) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < size && i < most; ++i) out += digits[data[i] >> 4], out += digits[data[i] & 15];
    return out;
}

// An event as its line (with its newline):
//   <seconds> in <from> <seat> <first> <count> | local <seat> <frame> | taken <seat> <frame> | idle <seat> <frame> |
//   started <seats> <seat> <frame> | note <text> | delay <frames> from <frame> | msg <from> <type> <size> <hex> |
//   sent <to> <type> <size> <hex> | sync ahead <frames> behind <frames> slowed <n> sooner <n> unpaced <n>
inline std::string line(const Event& e) {
    char text[256];
    switch (e.kind) {
    case Kind::inputs_in:
        std::snprintf(text, sizeof text, "%.4f in %u %u %llu %llu\n", e.t, e.from + 1u, e.seat + 1u, static_cast<unsigned long long>(e.a),
                      static_cast<unsigned long long>(e.b));
        break;
    case Kind::local: std::snprintf(text, sizeof text, "%.4f local %u %llu\n", e.t, e.seat + 1u, static_cast<unsigned long long>(e.a)); break;
    case Kind::taken: std::snprintf(text, sizeof text, "%.4f taken %u %llu\n", e.t, e.seat + 1u, static_cast<unsigned long long>(e.a)); break;
    case Kind::idle: std::snprintf(text, sizeof text, "%.4f idle %u %llu\n", e.t, e.seat + 1u, static_cast<unsigned long long>(e.a)); break;
    case Kind::started:
        std::snprintf(text, sizeof text, "%.4f started %llu %u %llu\n", e.t, static_cast<unsigned long long>(e.a), e.seat + 1u,
                      static_cast<unsigned long long>(e.b));
        break;
    case Kind::note:
        std::snprintf(text, sizeof text, "%.4f note ", e.t);
        return std::string(text) + e.text + "\n";
    case Kind::delay:
        std::snprintf(text, sizeof text, "%.4f delay %llu from %llu\n", e.t, static_cast<unsigned long long>(e.a), static_cast<unsigned long long>(e.b));
        break;
    case Kind::msg: // (its bytes can be long: written apart)
    case Kind::sent:
        std::snprintf(text, sizeof text, "%.4f %s %u %llu %llu ", e.t, e.kind == Kind::msg ? "msg" : "sent", e.from + 1u,
                      static_cast<unsigned long long>(e.a), static_cast<unsigned long long>(e.b));
        return std::string(text) + e.text + "\n";
    case Kind::sync:
        std::snprintf(text, sizeof text, "%.4f sync ahead %llu behind %llu ", e.t, static_cast<unsigned long long>(e.a),
                      static_cast<unsigned long long>(e.b));
        return std::string(text) + e.text + "\n";
    default: text[0] = 0; break;
    }
    return text;
}
constexpr const char* kHeader = "# net-events 1: seconds since the run's first event; seats from 1; see src/net/event_log.hpp\n";

// The events of the last `seconds`, oldest first, a line each (empty: none).
inline std::string text(double seconds = 300) {
    detail::Ring& r = detail::ring();
    const std::vector<Event> all = detail::since(0);
    if (all.empty()) return {};
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - r.origin).count();
    std::string out = kHeader;
    for (const Event& e : all)
        if (e.t >= now - seconds) out += line(e);
    return out;
}

// ---- A network game's whole timeline, in a file beside its recording (NAME.events: tools/net_record.hpp's) ----
// The ring keeps ~5 minutes; a problem in a long game needs the whole of it (the owner, 2026-11-13). record_to(path)
// starts a file with what the ring holds now (the join's exchanges before the recording began) and a thread of its own
// adds each second's new events to it (the game never waits on the disk); record_to("") (the game left) finishes it.

namespace detail {
struct File {
    std::mutex lock;
    std::FILE* out = nullptr;
    uint64_t written = 0; // (the last event's seq in it)
    bool thread_started = false;
};
inline File& file() {
    static File* f = new File; // (never destroyed: its thread runs to the end)
    return *f;
}
// A file opened for writing from a UTF-8 path (Windows' fopen takes the ANSI code page: a folder named in Chinese fails).
inline std::FILE* open_utf8(const std::string& path) {
#ifdef _WIN32
    return _wfopen(std::filesystem::path(std::u8string(path.begin(), path.end())).wstring().c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}
inline void write_new(File& f) { // (the lock held)
    if (!f.out) return;
    const std::vector<Event> fresh = since(f.written);
    for (const Event& e : fresh) {
        const std::string l = line(e);
        std::fwrite(l.data(), 1, l.size(), f.out);
        f.written = e.seq;
    }
    if (!fresh.empty()) std::fflush(f.out);
}
} // namespace detail

inline void record_to(const std::string& path) {
    detail::File& f = detail::file();
    std::lock_guard guard(f.lock);
    if (f.out) { // (the last game's: its end written, closed)
        detail::write_new(f);
        std::fclose(f.out);
        f.out = nullptr;
    }
    if (path.empty()) return;
    f.out = detail::open_utf8(path); // (a player's folder can have any letters in its name)
    if (!f.out) return;
    std::fputs(kHeader, f.out);
    f.written = 0; // (from the oldest the ring still holds)
    detail::write_new(f);
    if (!f.thread_started) {
        f.thread_started = true;
        std::thread([&f] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::lock_guard g(f.lock);
                detail::write_new(f);
            }
        }).detach();
    }
}

} // namespace net::events

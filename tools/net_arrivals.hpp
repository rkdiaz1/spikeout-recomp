// A reported network game run again as the reporting PC had it (the owner, 2026-11-12: "a way to reproduce those
// problems from those reports, otherwise a fix won't be sure"): its timeline (net-events.txt, src/net/event_log.hpp)
// says when each other player's buttons came, against this PC's own frames. gamerun --net-replay REC --net-arrivals
// FILE puts each other seat's buttons for a frame (the recording's) into the input table only once this PC had made
// its own up to where it was when they came: rollback then guesses, finds its guesses wrong and goes back at the same
// frames as there. What can't be run again: the other PCs themselves, and the session's own exchanges (a take-over, a
// drop): those are in the timeline's notes (tools/net-timeline.ps1).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace tools::net_arrivals {

struct Arrivals {
    int me = -1;                    // this PC's seat (from 0)
    uint64_t first_local = 0, last_local = 0;
    // seat -> frame -> the last frame of this PC's own made when it came (its own made then, so put in before the read
    // that comes after making that frame's)
    std::map<int, std::map<uint64_t, uint64_t>> at;
    std::map<uint64_t, uint64_t> delays; // (this PC's input delay from a frame on: the timeline's "delay" lines)

    // The input delay this PC had at `frame` (`fallback` if the timeline says none).
    uint64_t delay_at(uint64_t frame, uint64_t fallback) const {
        auto it = delays.upper_bound(frame);
        return it == delays.begin() ? fallback : std::prev(it)->second;
    }

    // Each seat's frames come by `made` (this PC's own made up to that frame): put in by `put` (seat, frame) in order,
    // from `next` (seat: the table's next frame of it). Frames before the timeline begins (it keeps the last ~5 minutes;
    // the recording may begin sooner), from `first_frame` on: known already, put in at once.
    template <class Put, class Next>
    void put_by(uint64_t made, uint64_t first_frame, Next next, Put put) const {
        for (const auto& [seat, frames] : at) {
            if (seat == me || frames.empty()) continue;
            uint64_t f = std::max(next(seat), first_frame);
            for (; f < frames.begin()->first; ++f) put(seat, f);
            for (auto it = frames.lower_bound(f); it != frames.end() && it->second <= made; ++it) put(seat, it->first);
        }
    }

    // Reads net-events.txt; "" or why not.
    std::string load(const std::string& path) {
        std::ifstream in(path);
        if (!in) return "can't read " + path;
        std::vector<std::pair<double, uint64_t>> own;           // (when, frame made)
        std::map<int, std::map<uint64_t, double>> came;           // seat -> frame -> first came
        for (std::string line; std::getline(in, line);) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream l(line);
            double t = 0;
            std::string kind;
            if (!(l >> t >> kind)) continue;
            if (kind == "delay") {
                uint64_t d = 0, f = 0;
                std::string from_word;
                if (l >> d >> from_word >> f) delays[f] = d;
            } else if (kind == "local") {
                int seat = 0;
                uint64_t f = 0;
                if (l >> seat >> f) me = seat - 1, own.push_back({t, f});
            } else if (kind == "in") {
                int from = 0, seat = 0;
                uint64_t f = 0, n = 0;
                if (l >> from >> seat >> f >> n) {
                    auto& c = came[seat - 1];
                    for (uint64_t g = f; g < f + n; ++g) c.emplace(g, t); // (the first time only)
                }
            }
        }
        if (own.empty() || me < 0) return "no frames of this PC's own in " + path + " (a spectator's, or an empty timeline)";
        std::sort(own.begin(), own.end());
        // (this PC's own made up to: the most made by then)
        std::vector<std::pair<double, uint64_t>> upto;
        uint64_t most = 0;
        for (const auto& [t, f] : own) most = std::max(most, f), upto.push_back({t, most});
        first_local = own.front().second, last_local = most;
        // (tests: NET_ARRIVALS_LATE=N, each other seat's buttons come N frames later than they did: guesses made and
        // gone back over, to prove the replay's corrections exact)
        const char* late_env = std::getenv("NET_ARRIVALS_LATE");
        const uint64_t late = late_env ? std::strtoull(late_env, nullptr, 10) : 0;
        for (const auto& [seat, frames] : came) {
            if (seat == me) continue;
            auto& a = at[seat];
            for (const auto& [frame, t] : frames) {
                auto it = std::upper_bound(upto.begin(), upto.end(), std::pair<double, uint64_t>(t, ~uint64_t{0}));
                uint64_t made = it == upto.begin() ? 0 : std::prev(it)->second;
                // (made later by the test switch: never past where this PC would have stopped to wait for it, rollback's
                // window and its delay on; a real timeline never is: what came during a wait came at the frame it stood at)
                if (late) made = std::min(made + late, frame + 12 + delay_at(frame, 0));
                a[frame] = made;
            }
        }
        return {};
    }
};

} // namespace tools::net_arrivals

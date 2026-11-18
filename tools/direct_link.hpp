// Direct connect (docs/direct-connect.md): the simulated network board's link over a net::DirectRoom, the same for
// the window and gamerun (the owner's rule: they never differ in network play).
#pragma once

#include "net/direct.hpp"
#include "net/lobby.hpp"
#include "runtime/netboard.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <memory>
#include <stdexcept>
#include <vector>

namespace tools {

// The boards' link test round the ring (SimNetBoard's Testing) as ring messages, each frame's table in one exchange
// (gather). Every receive waits for the others (the board stalls); empty: the room ended.
class DirectNetLink : public m3::NetLink {
public:
    explicit DirectNetLink(net::DirectRoom& room) : room_(room) {}
    void send(const uint8_t* data, std::size_t size) override { room_.ring_send(data, size); }
    std::vector<uint8_t> receive() override {
        std::vector<uint8_t> in;
        if (!room_.ring_receive(in)) return {};
        return in;
    }
    const std::vector<uint8_t>* peek() override { return nullptr; }
    bool loopback() const override { return false; }
    bool gathers() const override { return true; }
    bool gather(const uint8_t* own, std::size_t size, std::vector<std::vector<uint8_t>>& by_distance) override {
        return room_.gather(own, size, by_distance);
    }
    void state(m3::State&) override { throw std::runtime_error("save state: not in a direct connect game"); }

private:
    net::DirectRoom& room_;
};

// The board for a seat of a direct room: the simulated board on the room's link.
inline std::unique_ptr<m3::NetBoardDevice> direct_netboard(net::DirectRoom& room) {
    return std::make_unique<m3::SimNetBoard>(std::make_unique<DirectNetLink>(room));
}

// A link's grade before Ready (docs/direct-connect.md; limits to tune on real links): good (the arcade's speed),
// warn (Ready asks first: the game may slow now and then), too far (no Ready), none (no direct connection: no Ready).
enum class LinkGrade { measuring, good, warn, too_far, none };
constexpr float kGoodOneWayMs = 5, kGoodSpreadMs = 2, kFarOneWayMs = 10, kFarSpreadMs = 5;
inline LinkGrade grade(const net::DirectRoom::Link& l) {
    if (!l.direct) return LinkGrade::none;
    if (l.one_way_ms < 0) return LinkGrade::measuring;
    if (l.one_way_ms > kFarOneWayMs || l.spread_ms > kFarSpreadMs) return LinkGrade::too_far;
    if (l.one_way_ms > kGoodOneWayMs || l.spread_ms > kGoodSpreadMs) return LinkGrade::warn;
    return LinkGrade::good;
}
inline const char* grade_text(LinkGrade g) {
    switch (g) {
    case LinkGrade::measuring: return "MEASURING";
    case LinkGrade::good: return "GOOD";
    case LinkGrade::warn: return "MAY SLOW";
    case LinkGrade::too_far: return "TOO FAR";
    default: return "NOT CONNECTED";
    }
}
// A link's signal bars (the room's rows and the in-game list alike, the owner's choice): 4 under half the good limit,
// 3 good, 2 may slow, 1 too far; 0 not measured or not connected.
inline int link_bars(const net::DirectRoom::Link& l) {
    switch (grade(l)) {
    case LinkGrade::good: return l.one_way_ms < kGoodOneWayMs / 2 ? 4 : 3;
    case LinkGrade::warn: return 2;
    case LinkGrade::too_far: return 1;
    default: return 0;
    }
}

// A link's bar in the room's rows (the owner's choice, 2026-11-18: the glossy bar, full for a short link): its length,
// percent (100 at 0 ms, 40 at the far limit, 8 at least); its colour by its grade.
inline int link_bar(const net::DirectRoom::Link& l) {
    return std::clamp(static_cast<int>(100.0f - l.one_way_ms * 6.0f), 8, 100);
}

// The worst grade of this seat's links (what its Ready goes by).
inline LinkGrade my_grade(const net::DirectRoom::View& v) {
    LinkGrade worst = LinkGrade::good;
    for (uint8_t s = 0; s < v.seats; ++s)
        if (s != v.my_seat && v.my_seat < v.seats && v.seat[s].present) worst = std::max(worst, grade(v.links[v.my_seat][s]));
    return worst;
}

// The room's build id: the game build's, marked as direct connect's (never meets a Mode B game).
inline uint64_t direct_build_id(uint64_t build) { return build ^ 0xD12EC7C0'4EC71D00ull; }

// ---- Internet rooms, through our lobby (its kind "direct": a list of their own; docs/direct-connect.md) ----
// Our router's address (Cloudflare's STUN) found, waiting up to `ms` for it (never a relay: direct connections only).
inline void direct_find_address(net::DirectRoom& room, int ms = 2000) {
    if (const auto stun = net::resolve("stun.cloudflare.com")) room.find_public_address({*stun, 3478});
    for (int waited = 0; waited < ms && !room.public_address(); waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

struct DirectLobby {
    net::LobbyClient lobby;
    std::string code;     // the room's (hosting)
    int told_taken = 1;   // (the host: what the lobby last heard)
    bool told_started = false;
};

inline std::string direct_hex(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

inline net::Json direct_address_list(const std::vector<net::Address>& list) {
    net::Json out = net::Json::array();
    for (const net::Address& a : list) out.push(a.str());
    return out;
}

inline bool direct_lobby_hello(DirectLobby& l, uint64_t build, const std::string& name, const std::string& url, std::string& why) {
    if (!net::LobbyClient::available()) return why = "this build has no internet lobby", false;
    l.lobby.connect(direct_hex(build), name, url, "direct");
    const auto welcome = l.lobby.wait_for("welcome", 10000);
    if (!welcome || (*welcome)["t"].as_string() != "welcome")
        return why = l.lobby.error().empty() ? "can't reach the lobby" : l.lobby.error(), false;
    return true;
}

// Host a room of `seats` on the internet (listed, or private: by its code only): the room opened, our addresses found,
// the lobby told; its code in l.code. Blocks up to some seconds (a thread's work in the window).
inline bool direct_lobby_host(DirectLobby& l, net::DirectRoom& room, uint64_t build, const std::string& name, uint8_t seats,
                              bool listed, std::string& why, const std::string& url = net::kLobbyUrl) {
    if (!direct_lobby_hello(l, build, name, url, why)) return false;
    if (!room.host(name, seats)) return why = room.error(), false;
    direct_find_address(room);
    net::Json m = net::Json::object();
    m.set("t", "host"), m.set("name", name), m.set("seats", static_cast<int>(seats)), m.set("public", listed);
    m.set("session", ""), m.set("port", static_cast<int>(room.port())), m.set("candidates", direct_address_list(room.candidates()));
    l.lobby.send(m);
    const auto hosted = l.lobby.wait_for("hosted", 10000);
    if (!hosted || (*hosted)["t"].as_string() != "hosted")
        return why = hosted ? (*hosted)["why"].as_string() : std::string("the lobby didn't answer"), false;
    l.code = (*hosted)["code"].as_string();
    return true;
}

// The host's part, now and then: players the lobby says are joining (our router opened to them); the room's seats
// taken and its start told to the lobby (its list).
inline void direct_lobby_pump(DirectLobby& l, net::DirectRoom& room) {
    while (auto m = l.lobby.next(0))
        if ((*m)["t"].as_string() == "joiner") {
            std::vector<net::Address> from;
            for (const net::Json& c : (*m)["candidates"].items())
                if (auto a = net::parse_address(c.as_string())) from.push_back(*a);
            room.expect(from);
        }
    const net::DirectRoom::View v = room.view();
    int taken = 0;
    for (uint8_t s = 0; s < v.seats; ++s) taken += v.seat[s].present ? 1 : 0;
    if (taken != l.told_taken || v.started != l.told_started) {
        l.told_taken = taken, l.told_started = v.started;
        net::Json m = net::Json::object();
        m.set("t", "update"), m.set("taken", taken), m.set("started", v.started);
        l.lobby.send(m);
    }
}

// Join a room by its code: our addresses found, the lobby asked, the host's addresses tried. Blocks some seconds.
inline bool direct_lobby_join(DirectLobby& l, net::DirectRoom& room, uint64_t build, const std::string& name,
                              const std::string& code, std::string& why, const std::string& url = net::kLobbyUrl) {
    if (!direct_lobby_hello(l, build, name, url, why)) return false;
    if (!room.open()) return why = room.error(), false;
    direct_find_address(room);
    net::Json m = net::Json::object();
    m.set("t", "join"), m.set("code", code), m.set("port", static_cast<int>(room.port()));
    m.set("candidates", direct_address_list(room.candidates()));
    l.lobby.send(m);
    const auto joining = l.lobby.wait_for("joining", 10000);
    if (!joining || (*joining)["t"].as_string() != "joining")
        return why = joining ? (*joining)["why"].as_string() : std::string("the lobby didn't answer"), false;
    std::vector<net::Address> hosts;
    for (const net::Json& c : (*joining)["candidates"].items())
        if (auto a = net::parse_address(c.as_string())) hosts.push_back(*a);
    if (!room.join(hosts, name)) return why = room.error(), false;
    return true;
}

} // namespace tools

// Direct connect's room and link (direct.hpp; docs/direct-connect.md).
#include "net/direct.hpp"
#include "net/stun.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace net {

namespace {
// The system's timer at 1 ms while a room is open (Windows' default step, 15.6 ms, would hold a datagram sent again,
// or a test's held one, that long): the room's network thread waits in steps of a millisecond.
struct FineTimer {
#ifdef _WIN32
    FineTimer() { timeBeginPeriod(1); }
    ~FineTimer() { timeEndPeriod(1); }
#endif
};
} // namespace

namespace {

using Clock = std::chrono::steady_clock;

// Datagram kinds (each starts "SD", the kind, then the room's id but HELLO's).
enum Kind : uint8_t {
    kHello = 1,  // joiner -> host: build, protocol, name, its own addresses
    kWelcome,    // host -> joiner: its seat, the seats
    kRefuse,     // host -> joiner: why not
    kRoster,     // host -> all, 4 a second: seats, names, ready, addresses, the links, started
    kReady,      // player -> host: ready or not
    kStats,      // player -> host: its links to the others
    kPing,       // player <-> player: the sender's time
    kPong,       // the ping's time back
    kSeg,        // in the game: a piece of a board's segment of exchange `seq`
    kAck,        // in the game: segments and ring messages had, up to (not including)
    kRing,       // in the game: a ring message (the boards' link test) to the next seat
    kBye,        // leaving
    kBeacon,     // host -> every network it is on (kDirectDiscoveryPort): build, room, name, seats, taken, started, port
    kPunch,      // host -> a joiner's addresses (the lobby's): only to open the host's router to them
};

constexpr std::size_t kPiece = 1200;                         // a segment's piece (a datagram's safe size)
constexpr auto kInGameLimit = std::chrono::seconds(10);      // a player waited for this long is gone
constexpr auto kQuietBeforeStart = std::chrono::seconds(5);  // a player heard nothing from before the start
constexpr std::size_t kSamples = 100;                        // round trips kept a pair
constexpr uint32_t kNone = 0xFFFFFFFF;

struct Writer {
    std::vector<uint8_t> out;
    explicit Writer(Kind k) { out = {'S', 'D', static_cast<uint8_t>(k)}; }
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { u8(static_cast<uint8_t>(v)), u8(static_cast<uint8_t>(v >> 8)); }
    void u32(uint32_t v) { u16(static_cast<uint16_t>(v)), u16(static_cast<uint16_t>(v >> 16)); }
    void u64(uint64_t v) { u32(static_cast<uint32_t>(v)), u32(static_cast<uint32_t>(v >> 32)); }
    void str(const std::string& s) {
        const std::size_t n = std::min<std::size_t>(s.size(), 255);
        u8(static_cast<uint8_t>(n));
        out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    }
    void addr(const Address& a) { u32(a.ip), u16(a.port); }
    void bytes(const uint8_t* p, std::size_t n) { out.insert(out.end(), p, p + n); }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    Reader(const std::vector<uint8_t>& d) : p(d.data() + 3), end(d.data() + d.size()) {}
    bool have(std::size_t n) {
        if (static_cast<std::size_t>(end - p) < n) ok = false;
        return ok;
    }
    uint8_t u8() { return have(1) ? *p++ : 0; }
    uint16_t u16() { const uint16_t lo = u8(); return static_cast<uint16_t>(lo | u8() << 8); }
    uint32_t u32() { const uint32_t lo = u16(); return lo | static_cast<uint32_t>(u16()) << 16; }
    uint64_t u64() { const uint64_t lo = u32(); return lo | static_cast<uint64_t>(u32()) << 32; }
    std::string str() {
        const std::size_t n = u8();
        if (!have(n)) return {};
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    Address addr() {
        Address a;
        a.ip = u32();
        a.port = u16();
        return a;
    }
    std::vector<uint8_t> rest() {
        std::vector<uint8_t> v(p, end);
        p = end;
        return v;
    }
};

uint64_t micros(Clock::time_point t) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count());
}

// Tests: SPIKEOUT_TEST_LATENCY=MS (fractions allowed) holds every datagram this machine sends that long, as a network
// that far away; SPIKEOUT_TEST_LOSS=PERCENT loses that many.
double test_latency_ms() {
    static const double ms = [] {
        const char* v = std::getenv("SPIKEOUT_TEST_LATENCY");
        return v ? std::strtod(v, nullptr) : 0.0;
    }();
    return ms;
}
double test_loss() {
    static const double loss = [] {
        const char* v = std::getenv("SPIKEOUT_TEST_LOSS");
        return v ? std::strtod(v, nullptr) / 100 : 0.0;
    }();
    return loss;
}

} // namespace

struct DirectRoom::Impl {
    FineTimer fine_timer;
    uint64_t build;
    std::function<void(const std::string&)> log;
    UdpSocket socket;
    std::thread thread;
    std::atomic<bool> quit{false};

    mutable std::mutex lock; // everything below
    std::condition_variable changed;
    bool hosting = false;
    uint64_t session = 0;
    uint8_t seats = 0, my_seat = 0xFF;
    bool started = false, stopped = false;
    bool want_ready = false; // (this player's Ready, sent with its measures until the start)
    std::string ended, my_name;
    Address host_address; // (joining: the host's address that answered; its candidates tried until one does)
    std::vector<Address> host_candidates;
    bool joining = false;
    Clock::time_point join_since{};
    // Internet rooms: our router's address (STUN), and the addresses we send to for a while to open our router
    // (the host, to a player the lobby says is joining)
    std::optional<Address> stun_server, public_addr;
    int stun_tries = 0;
    Clock::time_point stun_sent{};
    std::array<uint8_t, 12> stun_id{};
    struct Punch {
        std::vector<Address> to;
        Clock::time_point until;
    };
    std::vector<Punch> punches;
    Clock::time_point epoch = Clock::now();

    struct Peer {
        bool present = false, ready = false;
        std::string name;
        std::vector<Address> candidates; // where it may be reached
        std::optional<Address> address;  // where it answered from
        Clock::time_point heard{};
        std::deque<uint32_t> rtt_us;     // our round trips to it
        Link mine;                       // our measure of the link to it
        std::array<Link, kDirectSeats> reported; // (the host: its measures of the others, STATS)
        // In the game: our segments it hasn't acknowledged (seq -> the segment), and its, being put together / had.
        std::map<uint32_t, std::vector<uint8_t>> unacked;
        Clock::time_point sent_at{};     // when the oldest unacknowledged went (again)
        uint32_t acked = 0;              // it has ours up to (not including)
        std::map<uint32_t, std::vector<uint8_t>> parts; // (seq -> its segment, pieces filling it)
        std::map<uint32_t, std::vector<bool>> pieces;
        std::map<uint32_t, std::vector<uint8_t>> had;   // complete, not yet taken
        std::set<uint32_t> got;          // complete, past `complete`
        uint32_t complete = 0;          // we have its segments up to (not including)
        // Ring messages: ours to it (the next seat), its to us (the seat before).
        std::map<uint32_t, std::vector<uint8_t>> ring_unacked;
        uint32_t ring_acked = 0;
        std::map<uint32_t, std::vector<uint8_t>> ring_in;
        std::set<uint32_t> ring_got;
        uint32_t ring_complete = 0;
    };
    std::array<Peer, kDirectSeats> peer;
    std::vector<Address> my_candidates;

    uint32_t gather_seq = 0, ring_seq_out = 0, ring_seq_in = 0;
    uint8_t waiting_for = 0xFF;
    Clock::time_point waiting_since{};
    Counters counters;

    // Datagrams held back (SPIKEOUT_TEST_LATENCY), by the time they go.
    struct Held {
        Clock::time_point due;
        Address to;
        std::vector<uint8_t> data;
    };
    std::mutex send_lock;
    std::deque<Held> held;
    std::mt19937 loss_random{12345};

    void say(const std::string& s) {
        if (log) log(s);
    }

    // ---- Sending ----
    void send_raw(const Address& to, const std::vector<uint8_t>& data) {
        std::lock_guard g(send_lock);
        if (test_loss() > 0 && std::uniform_real_distribution<double>(0, 1)(loss_random) < test_loss()) return;
        if (const double ms = test_latency_ms(); ms > 0) {
            held.push_back({Clock::now() + std::chrono::microseconds(static_cast<int64_t>(ms * 1000)), to, data});
            return;
        }
        socket.send_to(to, data);
    }
    void send_held() {
        std::lock_guard g(send_lock);
        const auto now = Clock::now();
        while (!held.empty() && held.front().due <= now) {
            socket.send_to(held.front().to, held.front().data);
            held.pop_front();
        }
    }
    // (to a peer: its address once known, else every place it may be)
    void send_peer(uint8_t seat, const Writer& w) {
        const Peer& p = peer[seat];
        if (p.address) send_raw(*p.address, w.out);
        else
            for (const Address& a : p.candidates) send_raw(a, w.out);
    }
    Writer head(Kind k) const {
        Writer w(k);
        w.u64(session);
        return w;
    }

    // ---- Measures ----
    void measure(Peer& p) {
        if (p.rtt_us.empty()) return;
        std::vector<uint32_t> s(p.rtt_us.begin(), p.rtt_us.end());
        std::sort(s.begin(), s.end());
        const float median = static_cast<float>(s[s.size() / 2]) / 2000.0f;
        const float p95 = static_cast<float>(s[std::min(s.size() - 1, s.size() * 95 / 100)]) / 2000.0f;
        p.mine.one_way_ms = median;
        p.mine.spread_ms = p95 - median;
    }
    static Link worse(const Link& a, const Link& b) {
        Link l;
        l.direct = a.direct && b.direct;
        l.one_way_ms = std::max(a.one_way_ms, b.one_way_ms);
        l.spread_ms = std::max(a.spread_ms, b.spread_ms);
        if (a.one_way_ms < 0 || b.one_way_ms < 0) l.one_way_ms = std::max(a.one_way_ms, b.one_way_ms);
        return l;
    }

    // ---- The host's roster ----
    void send_roster() {
        Writer w = head(kRoster);
        w.u8(seats), w.u8(started ? 1 : 0);
        for (uint8_t s = 0; s < seats; ++s) {
            const Peer& p = peer[s];
            w.u8(p.present ? 1 : 0), w.u8(p.ready ? 1 : 0), w.str(p.name);
            std::vector<Address> c = p.candidates;
            if (s != 0 && p.address) c.insert(c.begin(), *p.address); // (as the host saw it first)
            if (c.size() > 8) c.resize(8);
            w.u8(static_cast<uint8_t>(c.size()));
            for (const Address& a : c) w.addr(a);
        }
        for (uint8_t a = 0; a < seats; ++a)
            for (uint8_t b = 0; b < seats; ++b) {
                const Link& l = peer[a].reported[b];
                w.u8(l.direct ? 1 : 0);
                w.u32(l.one_way_ms < 0 ? kNone : static_cast<uint32_t>(l.one_way_ms * 1000));
                w.u32(l.spread_ms < 0 ? kNone : static_cast<uint32_t>(l.spread_ms * 1000));
            }
        for (uint8_t s = 1; s < seats; ++s)
            if (peer[s].present) send_peer(s, w);
    }
    void read_roster(Reader& r) {
        const uint8_t n = r.u8();
        const bool go = r.u8() != 0;
        if (!r.ok || n != seats) return;
        for (uint8_t s = 0; s < n; ++s) {
            Peer& p = peer[s];
            p.present = r.u8() != 0;
            p.ready = r.u8() != 0;
            p.name = r.str();
            const uint8_t c = r.u8();
            std::vector<Address> cands;
            for (uint8_t i = 0; i < c && r.ok; ++i) cands.push_back(r.addr());
            if (s != 0 && s != my_seat) p.candidates = cands;
        }
        for (uint8_t a = 0; a < n; ++a)
            for (uint8_t b = 0; b < n; ++b) {
                Link l;
                l.direct = r.u8() != 0;
                const uint32_t ow = r.u32(), sp = r.u32();
                l.one_way_ms = ow == kNone ? -1 : static_cast<float>(ow) / 1000.0f;
                l.spread_ms = sp == kNone ? -1 : static_cast<float>(sp) / 1000.0f;
                if (r.ok) peer[a].reported[b] = l;
            }
        if (r.ok && go && !started) {
            started = true;
            say("direct: the room has started");
            changed.notify_all();
        }
    }

    // ---- Datagrams ----
    void handle(const UdpSocket::Datagram& d) {
        if (stun::is_stun(d.data)) { // (our router's address, as the STUN server sees our datagrams)
            const auto m = stun::parse(d.data);
            std::lock_guard g(lock);
            if (m && m->type == stun::kBindingSuccess && m->id == stun_id && !public_addr)
                if (const auto a = m->xor_address(stun::kXorMappedAddress)) {
                    public_addr = *a;
                    if (std::none_of(my_candidates.begin(), my_candidates.end(), [&](const Address& c) { return c.same_place(*a); }))
                        my_candidates.push_back(*a);
                    if (hosting) peer[0].candidates = my_candidates;
                    say("direct: our address on the internet found");
                }
            return;
        }
        if (d.data.size() < 3 || d.data[0] != 'S' || d.data[1] != 'D') return;
        Reader r(d.data);
        const Kind kind = static_cast<Kind>(d.data[2]);
        if (kind == kPunch) return; // (only to open a router)
        std::unique_lock g(lock);
        if (kind == kHello) {
            if (!hosting) return;
            const uint64_t b = r.u64();
            const uint8_t protocol = r.u8();
            const std::string name = r.str();
            std::vector<Address> cands;
            for (uint8_t i = 0, n = r.u8(); i < n && r.ok && i < 8; ++i) cands.push_back(r.addr());
            if (!r.ok) return;
            auto refuse = [&](const std::string& why) {
                Writer w(kRefuse);
                w.str(why);
                send_raw(d.from, w.out);
            };
            if (b != build || protocol != kDirectProtocol) return refuse("that room is on another version of the game");
            uint8_t seat = 0xFF;
            for (uint8_t s = 1; s < seats; ++s)
                if (peer[s].present && peer[s].address && peer[s].address->same_place(d.from)) seat = s;
            if (seat == 0xFF) {
                if (started) return refuse("that room's game has started");
                for (uint8_t s = 1; s < seats && seat == 0xFF; ++s)
                    if (!peer[s].present) seat = s;
                if (seat == 0xFF) return refuse("that room is full");
                Peer& p = peer[seat];
                p = Peer{};
                p.present = true, p.name = name, p.candidates = cands, p.address = d.from, p.heard = Clock::now();
                say("direct: " + name + " joined, seat " + std::to_string(seat + 1));
            }
            Writer w = head(kWelcome);
            w.u8(seat), w.u8(seats);
            send_raw(d.from, w.out);
            send_roster();
            return;
        }
        if (kind == kRefuse) {
            if (hosting || my_seat != 0xFF) return;
            ended = r.str();
            if (ended.empty()) ended = "the room said no";
            changed.notify_all();
            return;
        }
        const uint64_t sid = r.u64();
        if (kind == kWelcome) {
            if (hosting || my_seat != 0xFF ||
                std::none_of(host_candidates.begin(), host_candidates.end(), [&](const Address& a) { return a.same_place(d.from); }))
                return;
            const uint8_t seat = r.u8(), n = r.u8();
            if (!r.ok || seat == 0 || seat >= n || n < 2 || n > kDirectSeats) return;
            host_address = d.from; // (the host's address that answered)
            session = sid, my_seat = seat, seats = n;
            peer[0].present = true, peer[0].address = host_address, peer[0].candidates = {host_address};
            peer[0].heard = Clock::now();
            say("direct: in the room, seat " + std::to_string(seat + 1) + " of " + std::to_string(n));
            changed.notify_all();
            return;
        }
        if (sid != session || my_seat == 0xFF) return;
        if (kind == kRoster) {
            if (hosting) return;
            peer[0].heard = Clock::now();
            read_roster(r);
            return;
        }
        // (the rest: from a seat)
        const uint8_t from = r.u8();
        if (!r.ok || from >= seats || from == my_seat) return;
        Peer& p = peer[from];
        p.heard = Clock::now();
        if (!p.address) p.address = d.from; // (the first place it answered from)
        switch (kind) {
        case kReady:
            if (hosting) p.ready = r.u8() != 0;
            break;
        case kStats:
            if (!hosting) break;
            if (!started) p.ready = r.u8() != 0;
            else r.u8();
            for (uint8_t s = 0; s < seats && r.ok; ++s) {
                Link l;
                l.direct = r.u8() != 0;
                const uint32_t ow = r.u32(), sp = r.u32();
                l.one_way_ms = ow == kNone ? -1 : static_cast<float>(ow) / 1000.0f;
                l.spread_ms = sp == kNone ? -1 : static_cast<float>(sp) / 1000.0f;
                if (r.ok) p.reported[s] = l;
            }
            break;
        case kPing: {
            const uint64_t t = r.u64();
            if (!r.ok) break;
            Writer w = head(kPong);
            w.u8(my_seat), w.u64(t);
            send_raw(d.from, w.out);
            break;
        }
        case kPong: {
            const uint64_t t = r.u64(), now = micros(Clock::now());
            if (!r.ok || t > now) break;
            p.address = d.from;
            p.mine.direct = true;
            p.rtt_us.push_back(static_cast<uint32_t>(std::min<uint64_t>(now - t, 0xFFFFFFF0)));
            while (p.rtt_us.size() > kSamples) p.rtt_us.pop_front();
            measure(p);
            break;
        }
        case kSeg: {
            const uint32_t seq = r.u32();
            const uint8_t piece = r.u8(), pieces = r.u8();
            const uint16_t total = r.u16();
            std::vector<uint8_t> data = r.rest();
            if (!r.ok || pieces == 0 || piece >= pieces) break;
            if (seq >= p.complete && !p.got.count(seq)) {
                auto& buf = p.parts[seq];
                auto& got = p.pieces[seq];
                if (buf.size() != total) buf.assign(total, 0), got.assign(pieces, false);
                const std::size_t at = static_cast<std::size_t>(piece) * kPiece;
                if (at + data.size() <= buf.size() && got.size() == pieces) {
                    std::memcpy(buf.data() + at, data.data(), data.size());
                    got[piece] = true;
                }
                if (std::all_of(got.begin(), got.end(), [](bool b) { return b; })) {
                    p.had[seq] = std::move(buf);
                    p.parts.erase(seq), p.pieces.erase(seq);
                    p.got.insert(seq);
                    while (p.got.count(p.complete)) p.got.erase(p.complete++);
                    changed.notify_all();
                }
            }
            send_ack(from);
            break;
        }
        case kRing: {
            const uint32_t seq = r.u32();
            std::vector<uint8_t> data = r.rest();
            if (!r.ok) break;
            if (seq >= p.ring_complete && !p.ring_got.count(seq)) {
                p.ring_in[seq] = std::move(data);
                p.ring_got.insert(seq);
                while (p.ring_got.count(p.ring_complete)) p.ring_got.erase(p.ring_complete++);
                changed.notify_all();
            }
            send_ack(from);
            break;
        }
        case kAck: {
            const uint32_t seg = r.u32(), ring = r.u32();
            if (!r.ok) break;
            if (seg > p.acked) {
                p.acked = seg;
                p.unacked.erase(p.unacked.begin(), p.unacked.lower_bound(seg));
                p.sent_at = Clock::now();
            }
            if (ring > p.ring_acked) {
                p.ring_acked = ring;
                p.ring_unacked.erase(p.ring_unacked.begin(), p.ring_unacked.lower_bound(ring));
            }
            break;
        }
        case kBye:
            if (!started) {
                if (hosting) {
                    say("direct: " + p.name + " left the room");
                    p = Peer{};
                } else if (from == 0) {
                    ended = "the host closed the room";
                }
            } else {
                ended = p.name + " left the game";
            }
            changed.notify_all();
            break;
        default: break;
        }
    }
    void send_ack(uint8_t to) {
        Writer w = head(kAck);
        w.u8(my_seat), w.u32(peer[to].complete), w.u32(peer[to].ring_complete);
        send_peer(to, w);
    }

    // ---- The network thread ----
    void run() {
        auto last_ping = Clock::now(), last_roster = last_ping, last_stats = last_ping, last_beacon = last_ping - std::chrono::seconds(2),
             last_punch = last_ping;
        while (!quit) {
            socket.wait(1);
            while (auto d = socket.receive()) handle(*d);
            send_held();
            const auto now = Clock::now();
            std::unique_lock g(lock);
            if (stun_server && !public_addr && stun_tries < 10 && now - stun_sent > std::chrono::milliseconds(300)) {
                ++stun_tries, stun_sent = now;
                stun_id = stun::new_transaction();
                socket.send_to(*stun_server, stun::binding_request(stun_id));
            }
            if (!punches.empty() && now - last_punch > std::chrono::milliseconds(200)) { // (the host: routers opened)
                last_punch = now;
                std::erase_if(punches, [&](const Punch& p) { return now > p.until; });
                Writer w = head(kPunch);
                for (const Punch& p : punches)
                    for (const Address& a : p.to) send_raw(a, w.out);
            }
            if (my_seat == 0xFF) { // (joining: hello until welcomed, to every address the host may be at)
                if (joining && ended.empty() && now - join_since > std::chrono::seconds(15)) {
                    ended = "can't reach the host directly (a router lets nothing through; the arcade link never uses a relay)";
                    changed.notify_all();
                }
                if (joining && ended.empty() && now - last_ping > std::chrono::milliseconds(250)) {
                    last_ping = now;
                    Writer w(kHello);
                    w.u64(build), w.u8(kDirectProtocol), w.str(my_name);
                    w.u8(static_cast<uint8_t>(std::min<std::size_t>(my_candidates.size(), 8)));
                    for (std::size_t i = 0; i < my_candidates.size() && i < 8; ++i) w.addr(my_candidates[i]);
                    for (const Address& a : host_candidates) send_raw(a, w.out);
                }
                continue;
            }
            // Pings, 20 a second before the start, 10 in the game.
            if (now - last_ping > std::chrono::milliseconds(started ? 100 : 50)) {
                last_ping = now;
                for (uint8_t s = 0; s < seats; ++s)
                    if (s != my_seat && peer[s].present) {
                        Writer w = head(kPing);
                        w.u8(my_seat), w.u64(micros(now));
                        send_peer(s, w);
                    }
            }
            if (hosting && now - last_beacon > std::chrono::seconds(1)) { // (LAN discovery: DirectDiscovery)
                last_beacon = now;
                Writer w(kBeacon);
                w.u64(build), w.u64(session), w.str(my_name), w.u8(seats);
                uint8_t taken = 0;
                for (uint8_t s = 0; s < seats; ++s) taken = static_cast<uint8_t>(taken + (peer[s].present ? 1 : 0));
                w.u8(taken), w.u8(started ? 1 : 0), w.u16(socket.port());
                for (const Address& b : broadcast_addresses(kDirectDiscoveryPort)) send_raw(b, w.out);
            }
            if (hosting && now - last_roster > std::chrono::milliseconds(250)) {
                last_roster = now;
                for (uint8_t s = 1; s < seats; ++s) // (gone quiet before the start: its seat free again)
                    if (!started && peer[s].present && now - peer[s].heard > kQuietBeforeStart) {
                        say("direct: " + peer[s].name + " went quiet: let go");
                        peer[s] = Peer{};
                    }
                for (uint8_t s = 1; s < seats; ++s) peer[0].reported[s] = peer[s].mine;
                send_roster();
            }
            if (!hosting && now - last_stats > std::chrono::milliseconds(250)) {
                last_stats = now;
                Writer w = head(kStats);
                w.u8(my_seat), w.u8(want_ready ? 1 : 0);
                for (uint8_t s = 0; s < seats; ++s) {
                    const Link& l = peer[s].mine;
                    w.u8(l.direct ? 1 : 0);
                    w.u32(l.one_way_ms < 0 ? kNone : static_cast<uint32_t>(l.one_way_ms * 1000));
                    w.u32(l.spread_ms < 0 ? kNone : static_cast<uint32_t>(l.spread_ms * 1000));
                }
                send_peer(0, w);
                if (!started && ended.empty() && now - peer[0].heard > kQuietBeforeStart) {
                    ended = "the host went quiet";
                    changed.notify_all();
                }
            }
            // In the game: what isn't acknowledged in time, again.
            if (started)
                for (uint8_t s = 0; s < seats; ++s) {
                    Peer& p = peer[s];
                    if (s == my_seat || (p.unacked.empty() && p.ring_unacked.empty())) continue;
                    const double rtt_ms = p.mine.one_way_ms > 0 ? 2 * p.mine.one_way_ms : 10;
                    const auto wait = std::chrono::microseconds(static_cast<int64_t>(std::max(3.0, rtt_ms * 1.5 + 2) * 1000));
                    if (now - p.sent_at < wait) continue;
                    p.sent_at = now;
                    for (const auto& [seq, seg] : p.unacked) send_segment(s, seq, seg), ++counters.resent;
                    for (const auto& [seq, msg] : p.ring_unacked) send_ring(s, seq, msg), ++counters.resent;
                }
        }
    }
    void send_segment(uint8_t to, uint32_t seq, const std::vector<uint8_t>& seg) {
        const std::size_t pieces = std::max<std::size_t>(1, (seg.size() + kPiece - 1) / kPiece);
        for (std::size_t i = 0; i < pieces; ++i) {
            Writer w = head(kSeg);
            w.u8(my_seat), w.u32(seq), w.u8(static_cast<uint8_t>(i)), w.u8(static_cast<uint8_t>(pieces));
            w.u16(static_cast<uint16_t>(seg.size()));
            const std::size_t at = i * kPiece;
            w.bytes(seg.data() + at, std::min(kPiece, seg.size() - std::min(at, seg.size())));
            send_peer(to, w);
        }
    }
    void send_ring(uint8_t to, uint32_t seq, const std::vector<uint8_t>& msg) {
        Writer w = head(kRing);
        w.u8(my_seat), w.u32(seq);
        w.bytes(msg.data(), msg.size());
        send_peer(to, w);
    }

    // (waiting in the game: for seat `seat`'s data, until `ready`; false: the room ended or stopped)
    template <typename Ready>
    bool wait_for(std::unique_lock<std::mutex>& g, uint8_t seat, Ready ready) {
        const auto start = Clock::now();
        waiting_for = seat, waiting_since = start;
        while (!ready()) {
            if (stopped || !ended.empty()) {
                waiting_for = 0xFF;
                return false;
            }
            if (Clock::now() - start > kInGameLimit) {
                ended = peer[seat].name + "'s connection was lost";
                say("direct: " + ended);
                changed.notify_all();
                waiting_for = 0xFF;
                return false;
            }
            changed.wait_for(g, std::chrono::milliseconds(100));
        }
        waiting_for = 0xFF;
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        counters.waited_s += ms / 1000;
        if (seat < kDirectSeats) counters.waited_by[seat] += ms / 1000;
        counters.longest_ms = std::max(counters.longest_ms, ms);
        return true;
    }
};

DirectRoom::DirectRoom(uint64_t build, std::function<void(const std::string&)> log) : impl_(std::make_unique<Impl>()) {
    impl_->build = build;
    impl_->log = std::move(log);
}

DirectRoom::~DirectRoom() {
    leave();
    impl_->quit = true;
    if (impl_->thread.joinable()) impl_->thread.join();
}

static std::vector<Address> own_candidates(uint16_t port) {
    std::vector<Address> out;
    for (uint32_t ip : local_addresses()) out.push_back(Address{ip, port});
    return out;
}

bool DirectRoom::open(uint16_t port) {
    Impl& m = *impl_;
    if (m.socket.is_open()) return true;
    if (!m.socket.open(port)) return error_ = m.socket.error(), false;
    {
        std::lock_guard g(m.lock);
        m.my_candidates = own_candidates(m.socket.port());
    }
    m.thread = std::thread([&m] { m.run(); });
    return true;
}

void DirectRoom::find_public_address(const Address& stun_server) {
    std::lock_guard g(impl_->lock);
    impl_->stun_server = stun_server;
}

std::optional<Address> DirectRoom::public_address() const {
    std::lock_guard g(impl_->lock);
    return impl_->public_addr;
}

std::vector<Address> DirectRoom::candidates() const {
    std::lock_guard g(impl_->lock);
    return impl_->my_candidates;
}

void DirectRoom::expect(const std::vector<Address>& addresses) {
    std::lock_guard g(impl_->lock);
    impl_->punches.push_back({addresses, Clock::now() + std::chrono::seconds(15)});
}

bool DirectRoom::host(const std::string& name, uint8_t seats, uint16_t port) {
    Impl& m = *impl_;
    if (seats < 2 || seats > kDirectSeats) return error_ = "2 to 4 players", false;
    if (!open(port)) return false;
    std::random_device rd;
    std::lock_guard g(m.lock);
    m.session = (static_cast<uint64_t>(rd()) << 32) | rd();
    m.hosting = true, m.seats = seats, m.my_seat = 0, m.my_name = name;
    m.peer[0].present = true, m.peer[0].name = name;
    m.peer[0].candidates = m.my_candidates;
    m.say("direct: hosting a room of " + std::to_string(seats) + " on port " + std::to_string(m.socket.port()));
    return true;
}

bool DirectRoom::join(const Address& host, const std::string& name) { return join(std::vector<Address>{host}, name); }

bool DirectRoom::join(const std::vector<Address>& hosts, const std::string& name) {
    Impl& m = *impl_;
    if (hosts.empty()) return error_ = "no address for the room", false;
    if (!open(0)) return false;
    std::lock_guard g(m.lock);
    m.host_candidates = hosts, m.host_address = hosts.front(), m.my_name = name, m.joining = true;
    m.join_since = Clock::now();
    m.say("direct: joining " + hosts.front().str() + (hosts.size() > 1 ? " (or " + std::to_string(hosts.size() - 1) + " other addresses)" : ""));
    return true;
}

uint16_t DirectRoom::port() const { return impl_->socket.port(); }

DirectRoom::View DirectRoom::view() const {
    const Impl& m = *impl_;
    std::lock_guard g(m.lock);
    View v;
    v.seats = m.seats, v.my_seat = m.my_seat, v.started = m.started, v.ended = m.ended;
    for (uint8_t s = 0; s < m.seats; ++s) {
        v.seat[s].present = m.peer[s].present, v.seat[s].ready = m.peer[s].ready, v.seat[s].name = m.peer[s].name;
        if (s == m.my_seat) v.seat[s].name = m.my_name;
    }
    // (each pair: the worse of the two sides' measures: ours where we're one side, the host's list for the others)
    for (uint8_t a = 0; a < m.seats; ++a)
        for (uint8_t b = static_cast<uint8_t>(a + 1); b < m.seats; ++b) {
            Link la = m.peer[a].reported[b], lb = m.peer[b].reported[a];
            if (a == m.my_seat) la = m.peer[b].mine;
            if (b == m.my_seat) lb = m.peer[a].mine;
            v.links[a][b] = v.links[b][a] = Impl::worse(la, lb);
        }
    return v;
}

void DirectRoom::set_ready(bool ready) {
    Impl& m = *impl_;
    std::lock_guard g(m.lock);
    if (m.started) return;
    m.want_ready = ready;
    if (m.hosting) m.peer[0].ready = ready;
    if (!m.hosting && m.my_seat != 0xFF) {
        Writer w = m.head(kReady);
        w.u8(m.my_seat), w.u8(ready ? 1 : 0);
        for (int i = 0; i < 3; ++i) m.send_peer(0, w); // (and the roster says it back)
    }
}

bool DirectRoom::can_start(std::string* why) const {
    const View v = view();
    auto no = [&](const std::string& s) {
        if (why) *why = s;
        return false;
    };
    if (v.my_seat != 0) return no("only the host starts the room");
    for (uint8_t s = 0; s < v.seats; ++s) {
        if (!v.seat[s].present) return no("seat " + std::to_string(s + 1) + " is empty");
        if (!v.seat[s].ready) return no(v.seat[s].name + " isn't ready");
    }
    for (uint8_t a = 0; a < v.seats; ++a)
        for (uint8_t b = static_cast<uint8_t>(a + 1); b < v.seats; ++b)
            if (!v.links[a][b].direct)
                return no(v.seat[a].name + " and " + v.seat[b].name + " can't connect directly");
    return true;
}

bool DirectRoom::start() {
    if (!can_start()) return false;
    Impl& m = *impl_;
    std::lock_guard g(m.lock);
    m.started = true;
    m.send_roster();
    m.say("direct: the room has started");
    m.changed.notify_all();
    return true;
}

bool DirectRoom::started() const {
    std::lock_guard g(impl_->lock);
    return impl_->started;
}

void DirectRoom::ring_send(const uint8_t* data, std::size_t size) {
    Impl& m = *impl_;
    std::lock_guard g(m.lock);
    const uint8_t next = static_cast<uint8_t>((m.my_seat + 1) % m.seats);
    Impl::Peer& p = m.peer[next];
    const uint32_t seq = m.ring_seq_out++;
    std::vector<uint8_t> msg(data, data + size);
    if (p.ring_unacked.empty() && p.unacked.empty()) p.sent_at = Clock::now();
    m.send_ring(next, seq, msg);
    p.ring_unacked[seq] = std::move(msg);
}

bool DirectRoom::ring_receive(std::vector<uint8_t>& out) {
    Impl& m = *impl_;
    std::unique_lock g(m.lock);
    const uint8_t before = static_cast<uint8_t>((m.my_seat + m.seats - 1) % m.seats);
    Impl::Peer& p = m.peer[before];
    const uint32_t seq = m.ring_seq_in;
    if (!m.wait_for(g, before, [&] { return p.ring_in.count(seq) != 0; })) return false;
    out = std::move(p.ring_in[seq]);
    p.ring_in.erase(seq);
    ++m.ring_seq_in;
    return true;
}

bool DirectRoom::gather(const uint8_t* own, std::size_t size, std::vector<std::vector<uint8_t>>& by_distance) {
    Impl& m = *impl_;
    std::unique_lock g(m.lock);
    const uint32_t seq = m.gather_seq;
    std::vector<uint8_t> mine(own, own + size);
    for (uint8_t s = 0; s < m.seats; ++s)
        if (s != m.my_seat) {
            Impl::Peer& p = m.peer[s];
            if (p.unacked.empty() && p.ring_unacked.empty()) p.sent_at = Clock::now();
            m.send_segment(s, seq, mine);
            p.unacked[seq] = mine;
        }
    by_distance.assign(m.seats - 1u, {});
    bool waited = false;
    const auto start = Clock::now();
    for (uint8_t d = 1; d < m.seats; ++d) {
        const uint8_t s = static_cast<uint8_t>((m.my_seat + m.seats - d) % m.seats);
        Impl::Peer& p = m.peer[s];
        if (!p.had.count(seq)) {
            waited = true;
            if (!m.wait_for(g, s, [&] { return p.had.count(seq) != 0; })) return false;
        }
        by_distance[d - 1u] = std::move(p.had[seq]);
        p.had.erase(seq);
    }
    ++m.gather_seq;
    ++m.counters.gathers;
    if (waited && Clock::now() - start > std::chrono::milliseconds(1)) ++m.counters.stalled;
    return true;
}

std::pair<uint8_t, int> DirectRoom::waiting() const {
    const Impl& m = *impl_;
    std::lock_guard g(m.lock);
    if (m.waiting_for == 0xFF) return {0xFF, 0};
    return {m.waiting_for,
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m.waiting_since).count())};
}

DirectRoom::Counters DirectRoom::counters() const {
    std::lock_guard g(impl_->lock);
    return impl_->counters;
}

void DirectRoom::stop() {
    std::lock_guard g(impl_->lock);
    impl_->stopped = true;
    impl_->changed.notify_all();
}

void DirectRoom::leave() {
    Impl& m = *impl_;
    if (!m.socket.is_open()) return;
    std::lock_guard g(m.lock);
    if (m.my_seat == 0xFF) return;
    Writer w = m.head(kBye);
    w.u8(m.my_seat);
    for (uint8_t s = 0; s < m.seats; ++s)
        if (s != m.my_seat && m.peer[s].present)
            for (int i = 0; i < 3; ++i) m.send_peer(s, w);
    m.my_seat = 0xFF;
    m.stopped = true;
    m.changed.notify_all();
}

bool DirectDiscovery::open() { return socket_.open(kDirectDiscoveryPort, true); }

std::vector<DirectRoomInfo> DirectDiscovery::poll() {
    const auto now = Clock::now();
    while (auto d = socket_.receive()) {
        if (d->data.size() < 3 || d->data[0] != 'S' || d->data[1] != 'D' || d->data[2] != kBeacon) continue;
        Reader r(d->data);
        const uint64_t build = r.u64();
        DirectRoomInfo info;
        info.session = r.u64(), info.name = r.str(), info.seats = r.u8(), info.taken = r.u8(), info.started = r.u8() != 0;
        info.host = d->from;
        info.host.port = r.u16();
        if (!r.ok || build != build_) continue;
        auto it = std::find_if(rooms_.begin(), rooms_.end(), [&](const Heard& h) { return h.info.session == info.session; });
        if (it == rooms_.end()) rooms_.push_back({info, now});
        else *it = {info, now};
    }
    std::erase_if(rooms_, [&](const Heard& h) { return now - h.at > std::chrono::seconds(3); });
    std::vector<DirectRoomInfo> out;
    for (const Heard& h : rooms_) out.push_back(h.info);
    return out;
}

std::string DirectRoom::ended() const {
    std::lock_guard g(impl_->lock);
    return impl_->ended;
}

} // namespace net

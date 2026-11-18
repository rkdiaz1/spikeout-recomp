// Direct connect (docs/direct-connect.md): each PC runs only its own cabinet, and the cabinets' link goes between the
// PCs as the arcade's cable did, through the simulated network board (m3::SimNetBoard, Supermodel's). A room of 2-4
// players: the host's cabinet is the master (seat 0), the ring is the seats in order. Before the start every pair of
// players measures its link (one-way time, spread); everyone says Ready; the host starts the room once every pair is
// connected directly (never through a relay). In the game, two kinds of traffic:
//   - the boards' own link test (SimNetBoard's Testing: the master's check round the ring): small messages to the next
//     seat, in order (ring_send / ring_receive);
//   - each frame, each board's own segment of the link's table to every other PC in one hop (gather): the board fills
//     its table as Supermodel's ring passes would have (segment k from the cabinet k places behind), the same data in
//     the same frame, only the transport differs (the owner's choice B).
// Every receive waits: a late packet stalls the board (the game slows), it never breaks the link. A player gone (a
// goodbye, or 10 s waited for its data) ends the room for everyone (ended()).
//
// UDP, a thread of its own; everything sent in the game is acknowledged and sent again until it is.
// Thread-safe: the room calls from any thread, the link calls from the game thread.
#pragma once

#include "net/udp.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace net {

constexpr uint8_t kDirectProtocol = 1;
constexpr std::size_t kDirectSeats = 4;
constexpr uint16_t kDirectDiscoveryPort = 47327; // (a host's beacon, once a second, on every network it is on)

// A direct room on the networks this machine is on, as its host's beacon says.
struct DirectRoomInfo {
    uint64_t session = 0;
    std::string name; // the host's
    uint8_t seats = 0, taken = 0;
    bool started = false;
    Address host; // its game port
};

// Listens for direct rooms' beacons (LAN discovery, as net::Discovery for network play's games). Only rooms of the
// same build are listed.
class DirectDiscovery {
public:
    explicit DirectDiscovery(uint64_t build) : build_(build) {}
    bool open();
    const std::string& error() const { return socket_.error(); }
    // The rooms heard from in the last few seconds (call it now and then).
    std::vector<DirectRoomInfo> poll();

private:
    uint64_t build_;
    UdpSocket socket_;
    struct Heard {
        DirectRoomInfo info;
        std::chrono::steady_clock::time_point at;
    };
    std::vector<Heard> rooms_;
};

class DirectRoom {
public:
    // A link between two players, as measured before (and during) the game.
    struct Link {
        bool direct = false;     // they reach each other (straight: never a relay)
        float one_way_ms = -1;   // half the round trip, the median of the last samples (-1: not measured yet)
        float spread_ms = -1;    // how unsteady: the 95th percentile's half round trip less the median's
    };
    struct Seat {
        bool present = false;
        bool ready = false;
        std::string name;
    };
    struct View {
        uint8_t seats = 0;
        uint8_t my_seat = 0xFF; // (0xFF: not seated yet)
        bool started = false;
        std::array<Seat, kDirectSeats> seat;
        // links[a][b], a < b: the pair's link (each side's measure, the worse of the two)
        std::array<std::array<Link, kDirectSeats>, kDirectSeats> links;
        std::string ended; // why the room ended ("" while it goes on)
    };

    explicit DirectRoom(uint64_t build, std::function<void(const std::string&)> log = {});
    ~DirectRoom();
    DirectRoom(const DirectRoom&) = delete;
    DirectRoom& operator=(const DirectRoom&) = delete;

    // The game port opened first (0: any), to find our internet address before going to the lobby (host and join
    // open it themselves if not).
    bool open(uint16_t port = 0);
    // Internet rooms: our router's address as a STUN server sees it, found in the background (never a relay).
    void find_public_address(const Address& stun_server);
    std::optional<Address> public_address() const;
    // Every address another player may reach us at (ours on our networks, our router's), with our game port.
    std::vector<Address> candidates() const;
    // The host: a player joining from these addresses (the lobby says): datagrams to them for a while open our router
    // to theirs.
    void expect(const std::vector<Address>& addresses);
    // A room of `seats` cabinets (2-4) on game port `port` (0: any); the host is seat 0.
    bool host(const std::string& name, uint8_t seats, uint16_t port = 0);
    // Join the room at `host` (or at whichever of `hosts` answers: the lobby's list of the host's addresses).
    bool join(const Address& host, const std::string& name);
    bool join(const std::vector<Address>& hosts, const std::string& name);
    const std::string& error() const { return error_; }
    uint16_t port() const;

    View view() const;
    // This player ready (or not any more), before the start.
    void set_ready(bool ready);
    // The host: whether the room can start (every seat taken, ready, every pair connected directly), and why not.
    bool can_start(std::string* why = nullptr) const;
    // The host: start the room (false: it can't, can_start says why).
    bool start();
    bool started() const;

    // ---- The link, from the game thread ----
    // The boards' own link test: to the next seat, in order; and the next from the seat before (waits).
    void ring_send(const uint8_t* data, std::size_t size);
    bool ring_receive(std::vector<uint8_t>& out);
    // Each frame: our board's own segment to every other player, and theirs of the same frame (waits for all):
    // by_distance[d - 1] is the segment of the seat d places before ours on the ring (d = 1 .. seats - 1). false: the
    // room ended (or stop()).
    bool gather(const uint8_t* own, std::size_t size, std::vector<std::vector<uint8_t>>& by_distance);
    // The seat the link is waiting for now (0xFF: none) and for how many milliseconds.
    std::pair<uint8_t, int> waiting() const;
    struct Counters {
        uint64_t gathers = 0;     // frames exchanged
        uint64_t stalled = 0;     // of them, waited over 1 ms
        double waited_s = 0;      // time waited in all
        double longest_ms = 0;    // the longest wait
        uint64_t resent = 0;      // datagrams sent again (not acknowledged in time)
        std::array<double, kDirectSeats> waited_by{}; // time waited (s), by the seat waited for
    };
    Counters counters() const;

    // Unblock the link's waits for good (quitting).
    void stop();
    // Leave the room (a goodbye to the others).
    void leave();
    // Why the room ended ("" while it goes on).
    std::string ended() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

} // namespace net

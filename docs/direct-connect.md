# Arcade link mode: the arcade's link over the network (design, 2026-11-17)

The README's "arcade link mode"; players see it as **Arcade link** (Network play > Arcade link; the owner's choice,
2026-11-17: "direct connect" sounded like typing an address in, which the game never asks for). In the code and
the lobby it is still "direct" (`net::DirectRoom`, `--direct-host`, the lobby's kind "direct"): players never see
those. For players close to each other (a LAN, or a few milliseconds apart over the
internet): each PC runs only its own cabinet, and the cabinets' link goes between the PCs as the cable did. Nothing is
guessed and nothing is gone back over; there are no other cabinets to run, no inputs to send, and the PCs can never
go out of step, because each cabinet is its own, as in the arcade.

The owner's decisions so far: no every-board and no rollback in this mode; no joining after the start; its own rooms
in the lobby, for friends; everyone clicks Ready and the game starts together; direct connections only, never the
relay (if two players who must talk can't connect directly, the room says so and doesn't start); each link's one-way
time and steadiness shown before Ready, Ready warned or refused over the limit; a late packet stalls the board (the
game slows), it never breaks the link. Our lobby, address finding (STUN) and LAN discovery are reused: nobody types
an address.

## Step 1: the simulated board against the Communication error (done, 2026-11-17)

The real network board (`m3::CommBoard`, the firmware's own 68000 code) makes several round trips a frame and
tolerates about 4 ms one way (Mode A in netplay.md). The mode therefore uses the simulated board, `m3::SimNetBoard`
(Supermodel's SimNetBoard, one pass of the ring a frame). Before building on it, it was run against the
Communication error found on 2026-11-16/17 (fidelity.md), on the preview6 problem bundles' games
(`Z:\Emu\spikeout-reports\replay`: 122 games, each its `.inputs` and `.state`, 4 cabinets, real players' presses).

How (gamerun, test code only):
- The states were saved with the real board. `--netboard sim` with a linked state now loads each board's state into
  the real board, then hands it to the simulation (`SimNetBoard::take_over`): the net RAM the game wrote, both comm
  RAM banks as the main board reads them, the link Ready. The real firmware's comm RAM header has Supermodel's layout
  (segment size 0xC00, the frame counter, status1 `0x2021 + 0x20 * others + index` the same on every board), so the
  hand-over is exact for what the game reads. A state from power-on (6 games, frame 3) gets the simulation from its
  start: it links the 4 cabinets itself.
- Supermodel's link is TCP: a receive waits for the previous cabinet's packet, so each frame's end waits for the
  others' (lockstep). gamerun's `BlockingRing` does the same between the boards' threads, with no time on the wire.
  Without the wait the simulated board breaks the link on its first frame (an empty receive is "the link broke").

Results (`--net-replay ... --netboard sim --watch-text "service staff" --watch-text "Communication error"`):

| network board | games with the Communication error |
|---|---|
| real, old link (before 2026-11-16) | 9 of 122 |
| real, the extra ring pass only | 9 of the first 40 |
| real, today's fix (ring pass + slave table late) | 0 of 122 |
| **simulated (Supermodel's), lockstep** | **0 of 122** |

Every one of the 122 ran to its recording's end with the simulated board, the link never broken; each board waited
well under a second in all for the others' packets. (The results: `Z:\Emu\spikeout-reports\replay\GAME\sim.txt`,
the table in `sim-summary.txt` there; build\sim-replays.ps1 in the worktree ran them.)

Staying in step with the recordings: the games recorded with RAM hashes part from the recording at some frame with
the real board too (58 of the 107 with hashes, the same number with either board). The replays aren't exact
copies of what ran on the players' PCs (rollback, one board's side runs and joins happened at moments the replay
doesn't reproduce), so after that frame they are a linked game of their own with the players' presses. Every frame
where the old link showed the error that could be checked comes after the replay had parted from the recording, with
either board: neither result is a replay of the exact moment. But the old link still shows the error in those parted
games, so they are hard enough to bring it out; the simulated board doesn't.

So the simulation is taken as it is (no timing change): the brief's rule (a slave shown the table one exchange
later, as today's fix) is kept for if real play shows the error.

## How a frame goes

Supermodel's simulated board, at the end of each main board frame (`SimNetBoard::run_frame`, state Ready): the
counter goes up, then for i = 0 .. cabinets-1 each cabinet sends the segment at `0x100 + i * size` of its board's
bank to the next cabinet and puts what it receives from the previous one at `0x100 + (i + 1) * size`; then the banks
swap. Each receive waits. After the loop, segment k of cabinet c is cabinet (c - k)'s own segment of this frame: every
cabinet has every cabinet's data of the same frame, which its game sees in the next frame.

### Decision 1 (owner): how the segments travel

**A. As Supermodel sends them**: the ring, cabinets-1 + 1 passes a frame, each waiting for the last. A frame's end
waits for about (cabinets) x (one-way time): 4 cabinets 4 ms apart wait 16 ms, a whole frame.

**B. The same data in one hop (recommended)**: each PC sends its own segment of the frame straight to every other PC
(the "pair" links of a full mesh, 3 KB each), and the board fills its segments as Supermodel's loop would have
(segment k from the cabinet k places behind it on the ring). What the game sees, and when (the same frame), is
exactly what A gives: only the transport differs (as Supermodel's own pull request 356 changed only transport). A
frame's end waits for about one one-way time.

Why it matters: a frame of one cabinet's work takes ~5 ms on the owner's PC (game logic ~3.3, 2D ~1.3; sound is on
the audio thread), and the window waits for the next frame anyway (the arcade runs 57.5 frames a second: 17.4 ms each). The link's wait comes out of that spare time, so
the game keeps the arcade's speed while (work + wait) stays under 17.4 ms. With B that is about 10 ms one way on a
fast PC (less on a slow one); with A, 4 cabinets, about 2.5 ms. B needs every pair of players to connect directly,
not only ring neighbours; a pair that can't is sent through a ring neighbour (one hop more for that pair, still
direct connections) and its time shown so.

Recommended: B, with A kept selectable for comparison (a test switch).

### Late and lost packets

UDP. Each frame's segment carries the frame number; every PC acknowledges what it has, and a segment not
acknowledged is sent again (after the measured round trip). A frame's end waits for every cabinet's segment of that
frame: the board stalls, the game slows, the link never breaks. A segment over a datagram's safe size (0xC00 bytes is
3 KB) goes in pieces; segments may be sent as their changes from the last frame (packed: zstd), the same bytes in the
end. Upload: 3 KB x 57.5 x (players - 1), ~520 KB/s with 4 players before packing.

### Decision 2 (owner): a player who is gone

A PC that sends nothing stalls every other cabinet for good (as the arcade with a cable pulled would show the error).
Proposed: after 1 s stalled, "WAITING FOR <name>" on the others' screens; after 10 s, or the player's goodbye, the room
ends for everyone with "<name>'s connection was lost" and back to the menu (or, the other choice: the link broken as
the cable pulled: the game's own Communication error).

## The room

- **Lobby**: its own room kind, listed apart ("Direct connect" in the menu, its own list); public with a code, or
  private by code only, for friends. This needs a change to the lobby Worker (a `kind` in host and list, joins of
  the right kind only): to be done and deployed only with the owner's word.
- **LAN**: the discovery beacon gets the same kind; a LAN room needs no lobby.
- **Addresses**: STUN and the lobby's candidates as today (`Session::find_public_address`, `expect`), the relay never
  asked for (`use_relay` not called; no TURN credentials used).
- **Seats**: 2-4, the host's cabinet the master (seat 0); the ring is the seats in order. Each player's own cabinet
  (EEPROM, backup RAM) is its own PC's, never sent.
- **Before Ready**: every pair pings 20 times a second; each link shows its one-way time (half the round trip, the
  median) and steadiness (the spread: 95th percentile minus median), each as green / yellow / red. Proposed limits
  (to tune by tests): green under 5 ms with spread under 2, red over 10 ms or spread over 5; Ready warns on yellow
  and is refused on red or on a pair with no direct connection.
- **Start**: once every seat is Ready, the host names a start moment (time sync as `tools::TimeSync`); every PC
  powers its cabinet on together. The boards' own link test (Supermodel's Testing: the master's check round the ring)
  then links them, waiting for each other as the cable did.
- **Leaving a game that has started** ends it for everyone (no cabinet can run on without its player's PC), so the
  player is warned first (the owner, 2026-11-17): "Leaving ends the game for everyone in the room. Leave?", Leave /
  Stay, before the goodbye is sent; the same for closing the window. The others then see "<name> left the game".
- **No joining after the start, no spectators, no save states (F9/F10), no rollback, no every-board.**
- **Overlay**: the seat list as today, each link's time and a stall count (frames that waited past their time).

## The code

- `m3::SimNetBoard` unchanged for the game (its Ready loop), with its receive waiting for the link (a `NetLink` whose
  receive waits, with a stop for quitting); transport B is a `NetLink` that hands the loop the right segment for each
  pass (the board stays Supermodel's).
- One shared piece for the window and gamerun (`tools/direct_link.hpp`: the room, Ready, the link, the stall), as the
  owner's rule says gamerun and the window never differ in network play; gamerun `--direct-host / --direct-join` for
  the tests, two or four on one PC first (`SPIKEOUT_TEST_LATENCY` for a distance), then the Codespaces and the owner's
  LAN.
- Builds: the mode's id includes the simulated board, so a build can only meet the same build.
- Network effect: none on today's network play (Mode B is untouched); this is a new mode beside it.

## As built (2026-11-17, uncommitted when written)

The owner's answers: transport B (one hop), and a gone player ends the room (10 s waited, or its goodbye).

- `net::DirectRoom` (src/net/direct.*): the room (hello / welcome / roster 4 a second, Ready sent with each player's
  measures until the start, the host's start once every seat is in, ready and every pair connected), the pairs'
  pings (20 a second before the start, 10 in the game; one-way time = half the median round trip, spread = the 95th
  percentile's less the median's), and the link: ring messages and each frame's segment, acknowledged and sent again
  after 1.5 round trips + 2 ms (3 ms at least). LAN discovery: the host's beacon once a second on port 47327
  (`net::DirectDiscovery`). On Windows the system timer is set to 1 ms while a room is open (the default 15.6 ms
  step held every resend that long). Tests: SPIKEOUT_TEST_LATENCY=MS (fractions allowed), SPIKEOUT_TEST_LOSS=PERCENT.
- `m3::NetLink::gather` and SimNetBoard's Ready: the one-hop table. Proved the same as Supermodel's ring passes:
  `SPIKEOUT_SIM_GATHER=1` on three replays (one linking up from power-on), all 634 RAM hashes equal.
- tools/direct_link.hpp (window and gamerun): the board on the room's link, the links' grades (GOOD under 5 ms one
  way and 2 ms spread, MAY SLOW, TOO FAR over 10 ms or 5 ms spread, NOT CONNECTED), the build id.
- gamerun `--direct-host SEATS[:PORT]`, `--direct-join ADDRESS:PORT | lan`, `--direct-wait MS` (ready at once; the
  host starts once every pair has been measured that long).
- The window: Network play > Direct connect (Host a room: players; the rooms on this network, joined with Enter);
  the room page (seats and READY, each link's time, spread and grade, Ready (refused on TOO FAR or NOT CONNECTED,
  warned on MAY SLOW), Start game for the host, Close / Leave room); once started, Leave game asks first, and closing
  the window does too. In the game: never paused by the menu, no F9/F10, "WAITING FOR <name>" after a second's wait,
  the counters in the log every 10 s; the room's end said at the bottom, the window back to its own game.
  Test switch: SPIKEOUT_TEST_DIRECT=host:N | join (ready at once, the host starting 2 s after everyone is in).

Tested on one PC:
- gamerun, 2 players: linked from power-on, 4,000 frames, no error.
- gamerun, 4 players found through LAN discovery, 2 ms each way and 1% of datagrams lost: links measured 2.6-2.7 ms,
  every cabinet linked to frame 3,000, ~500 datagrams sent again by each, no error.
- gamerun, a player leaving at frame 1,500: the host's room ended at once ("player left the game").
- Two windows (host + join by discovery): the link 0.05 ms GOOD, both from power-on to frame 2,500 at 57.52 frames a
  second (the arcade's); the host waited ~7 ms a frame for the joiner's segment, inside the frame's spare time.

Internet rooms (the owner's word, 2026-11-17; the lobby deployed that day, versions 79c1b5ea then 38691594):
- The lobby: hello's `kind: "direct"`; each kind its own list and "others" count; a join by code finds only a room
  of its own kind (a network play client with the same build never gets in: "that code is an Arcade link room", and
  the other way round). `npm test` 11 of 11.
- The room: `open()` first, our router's address from Cloudflare's STUN (`find_public_address`), every address in
  `candidates()`; the host opens its router to each joiner the lobby announces (`expect`: kPunch datagrams for 15 s);
  a joiner says hello to every address the lobby gave for the host and takes whichever answers; 15 s with no answer:
  the room ends, "can't reach the host directly (... the arcade link never uses a relay)".
- tools/direct_link.hpp: `direct_lobby_host` / `direct_lobby_join` / `direct_lobby_pump` (the host's seats and start
  told to the lobby), shared by gamerun (`--direct-host N --lobby URL [--direct-private]`, `--direct-join code:CODE`)
  and the window (Host a room > Where: This network / Internet (listed) / Internet (private); Join by code; the
  listed rooms under this network's, with country and estimated time; the room page shows its code).
- Tested on one PC through the live lobby: gamerun host + gamerun join by code, and window host + window join by
  code (SPIKEOUT_TEST_DIRECT=host:2:internet / code:CODE): linked, 2,000 and 1,800 frames, 57.52 frames a second.

The first test over the real internet (2026-11-17, the owner): a headless gamerun host on the UK South Codespace
(build\cs-arcade.sh: this worktree's sources synced there and built; build\cs-host.sh a fresh host, cs-stop.sh), an
internet room by code; the owner's window at home joined by its code. Connected directly (no relay), the link measured
3.87 ms one way, spread 1.49 ms (GOOD); both cabinets from power-on, linked, 16,430 frames (about 4 3/4 minutes) at
57.46 frames a second in the window (the arcade's 57.52), until the owner left ("DON left the game" on the host, at
once). The window waited 7.5 s in all (longest 285 ms: lost datagrams, ~820 sent again), the host about 16 ms a frame
inside its frame's spare time. The owner: it played fine, no Communication error. (Lesson: the host's 10-minute wait for players ran out before the first join: a fresh
host was started.)

The window's pages (the owner's choices from mockups, 2026-11-17/18; pictures were taken with build\direct-shots.ps1:
two windows, SPIKEOUT_TEST_DIRECT=host:N | join, SPIKEOUT_TEST_DIRECT_SHOW=1 opens the room page,
SPIKEOUT_TEST_DIRECT_HOLD=S holds the start, SPIKEOUT_TEST_ANY_DISTANCE=1 lets Ready through however far):
- The room page: a row a seat, each other player's link as the game's glossy bar (full and green for a short link,
  shorter and yellow or red as it slows: tools::link_bar), its time, a READY / WAITING pill; our own row's pill is our
  Ready (SET READY in an orange outline, Enter / A or a click); the host's Start a pill button, greyed with the
  reason; a joiner's greyed pill says what the room waits for (players, someone's Ready, the host's start). Under the
  rows the selected seat's links in words. Beside them the ring: always four seats, those not in the room grey, every
  pair a line coloured by its grade, the selected seat's thick with their times and a ping going out and back on
  each (ui::MenuPage::side_fx: a layer not made again with the page, so the animation runs on).
- In the game, the seat list as network play's (each player's time and signal bars) and under it one glossy bar: green
  "ARCADE SPEED", yellow "94% SPEED · KIM'S LINK" (frames in the last 3 s against the arcade's rate; the player
  waited for most, or "THE NETWORK"), red under 80%, red "WAITING FOR KIM · 6 S LEFT" counting down to the room's end;
  "LINKING THE CABINETS" at power-on. The bar shrinks towards the right.
- Measured with two windows on the i7-8700K: a simulated 13 ms each way still ran at 57.5 frames a second (each
  frame's work leaves time to wait); at 14 ms about 88-93%. The 10 ms "too far" limit is strict for fast PCs: to set
  after the owner's LAN test.

Not done yet: more tests between two networks (four players, and
routers stricter than a home's and a Codespace's) and on the owner's LAN between two PCs; a pair that can't connect sent through a
neighbour (refused now).

## Not decided / later

- The exact Ready limits (measure on real links first).
- A mode over a cable (Ethernet between two PCs) is the same thing on a LAN.
- Spectators of a direct game: not now.

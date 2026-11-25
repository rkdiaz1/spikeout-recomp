# Slow players: the plan (draft, 2026-11-24)

## Starting (hand-off for the session that builds it)

Read this file, then docs/netplay.md "Joins, big transfers and the link: the round of 2026-11-23/24" (what changed
just before: the seat's frame a multiple of 32, a joiner's state from several players, the transfers' pace, the
report request). The owner's rules that bind every step: gamerun and the window never differ in network play (one
shared code path, tools/*.hpp); every network change passes the full test list (both local suites, the spectator
tests, every internet case, the window tests, the report replays) before it is committed; say each change's network
effect; prove "same output" with hashes; commits only on the owner's word; the best design, never held back to stay
compatible with an older version (raise kProtocol).

Where each step works (the code as it stands):
- Catching up and "taken anyway": tools/net_frame.hpp `CatchUp::frame_end` (the "front"/"least" inputs, fix A's
  `kSpreadMost` and 1 s rule, the meeting `take_at` with `kMeetMost`, the "taken anyway" branch at "Not faster than
  the game"). Steps 2 and 4 replace most of it.
- Where the room is: tools/netplay.hpp `NetPlay::lead` (each other player's last input frame, half the round trip,
  their input delay) and `TimeSync` (the pace it drives). Step 2's room frame builds on `lead`, adding how long ago
  each player's last input came (Session's timing: `quiet_ms`).
- Taking the seat: src/net/session.hpp `Session::take_seat(at, frame, rate)` (kTake, the authority's seat frame,
  rounded to `kSeatFrameEvery`). Step 4's reserved seat is a seat that isn't taken: the authority already fills an
  open seat's inputs (`kOpenAhead`), and the lobby/status show seat states (`SeatView::State`).
- One board for a slow every-board joiner (step 3): tools/net_frame.hpp `AutoBoard` (`decide`, the window's Auto),
  and how a running machine switches (`one_board_due`, net_set_up_one_board in gamerun, the window's one_board_wanted).
- Cheaper one-board joins (step 5): tools/net_frame.hpp `JoinBases` (exact copies at the seat's frame) and
  tools/one_board.hpp (hand-backs, "held whole until the game is calm" after a set-up: ~50 s in an internet test,
  every rollback then re-running every board).
- Frame skip (step 6): the window's frame end, tools/spikeout.cpp `timing.on_frame_end` (the `unseen` path already
  draws nothing while catching up); `SPIKEOUT_NET_DEBUG=1` prints a frame's split every 300 frames (running, its end,
  waiting for inputs, side runs).
- Notices: the window's network status (bottom notices, the seat list); Session::log lines for gamerun.

Tests and tools: tools/onepc-tests.ps1, onepc-net-tests.ps1, spectate-tests.ps1, internet-tests.sh, the window tests
(net-window-matrix.ps1, spectate-window-test.ps1, onepc-window-test.ps1). A slow joiner on one PC:
`SPIKEOUT_TEST_CATCHUP_MS=N` (each frame caught up N ms longer); the room's longest wait for inputs is printed by every
gamerun ("longest wait for inputs", "Network: the longest a frame waited"). The players' reports: 131 "slow" and 4
"join" bundles of 2026-11-18..24 (each with the PC's numbers and the game's recording) to measure step 1 on real PCs.

The owner's rules (2026-11-23): both board modes; a player too slow for the room is moved to **watching**, never
kicked; the move is **automatic** (no question to the host); **both** the host and that player are told on screen,
when it happens and when the seat is taken back. The seat stays theirs while they watch (their character stands
idle, as on a disconnect).

Why: in the reports, about 5% of real catch-ups ran under 1.05x the game's speed. Today such a joiner is "taken
anyway" after 5 s: it gets its cabinet hundreds of frames ahead of itself, and the whole room stands still while it
runs them (3-16 s seen). A slow joiner in a one-board room ran the room at 19-30 fps (2026-11-23 tests).

## What a slow PC can and can't do (the facts the plan rests on)

- Catching up draws nothing, so it runs about 2.3x faster than playing (1.5x vs 0.64x on the slow test PC,
  docs/netplay.md "Catching up draws nothing"). A joiner that catches up at under 1.05x cannot play at full speed
  in that board mode at all: once it draws, it is slower still.
- A spectator runs every board too, at the room's pace (2 s behind). A PC too slow to play every board is too slow
  to watch every board. Watching only helps if it is cheaper than playing: on **one board** (spectators on one
  board exist: shadows from the players' backups), or with drawing thinned out (frame skip).
- So the order is: make the PC's work lighter first (one board, then frame skip). Watching is the last resort, and a
  watching slow PC watches on one board.

## Steps (in order; each one through the full network test list before the next)

### 1. Measure (no change to play)

On a weak setup, each board mode, both catching up and in play: per frame, game logic (every board, or one board
plus side runs), 2D, GPU frame, present, sound. Weak setups: this PC with the game held to 2 cores (CPU affinity),
`SPIKEOUT_TEST_CATCHUP_MS` (each frame N ms longer), and an Android or old-laptop number if one is at hand. gamerun
and the window both log the split once a second with `SPIKEOUT_FRAME_SPLIT=1` (new). Output: a table in this file,
which decides the thresholds below.

### 2. Where the room is (fix C; replaces fix A)

Today "caught up" and "too slow" are judged from the inputs received. Those keep pouring in for the seconds the
state was on its way, so a joiner gaining on the room can look like it's falling behind. Fix A (caught up once every
input-complete frame has been run for 1 s and the seats are within 30 frames) patched one symptom.

New: the room's frame is estimated from time sync. Each player's inputs carry its send time and frame. The room's
frame now = the median over the other players of (their last frame + the time since they sent it, in frames).

- **Caught up** = within (input delay + 4) frames of the room's frame.
- **Speed** = the gain on the room over the last 3 s (our frames run minus the room's progress), not the average
  since the start. A slow stretch no longer marks a joiner slow for good.
- **The meeting** = the room's frame + the gap / gain. Today's rule (ask for the cabinet from where we'll meet, the
  room playing on meanwhile) stays, with this better measure.

Fix A's limits (1 s, 30 frames) and the input-based "front" go.

### 3. A slow every-board joiner switches to one board

If its gain is under 5% of the game's speed after 3 s of running (step 2's measure), an every-board joiner switches to
one board before going on: the same switch as the window's Auto board mode under load. Its own cabinet's board stays
whole, the others are made from side runs and backups. It then catches up again. Told: "This PC is slow for every
board: one board from now on" (player only; it changes nobody else's game).

### 4. Watching until it keeps up (both modes; the safety net)

Still under 5% gain after the switch (or already on one board): the joiner does **not** take its cabinet. Instead:

- It stays in the room as a player-in-waiting: its seat reserved, its cabinet empty (the authority fills empty inputs,
  as for an open seat), its character standing idle. No new kind of peer: the seat simply isn't taken.
- It runs the game on one board, showing it (watching), at most the room's pace. Every other machine plays on:
  nobody waits for a seat that isn't taken.
- If it falls more than about 10 s behind the room, it asks the host for a fresh state (the join's state path) and
  starts from there, rather than keeping everyone's inputs for ever.
- It takes its seat once it has held at least 1.15x the game's speed with the frame skip of step 6 on for 10 s (spare
  time measured per frame while watching), then catches up as in step 2 and asks for its cabinet from the meeting
  frame.
- Told, on screen: to the player, "Watching: this PC can't keep up with the room yet. You'll join automatically when
  it can." To the host (and the room), "<name> is watching until their PC keeps up". Then "<name> took their seat"
  to everyone.

The "taken anyway" rule is removed. A joiner either meets the room or watches; the room never stands still for it.

### 5. Cheaper one-board joins

Today a one-board joiner runs every board whole until its player is in (about 80 s through a join). Hand the other
cabinets back to side runs as soon as the state is loaded and their bases are in (item 3's join bases give exact
copies at the seat frame). The 2026-11-22 late-joiner desync came from handing back early without those bases. With
them, the hand-back is at a known, exact frame. Proved by the node check and hashes against every board whole.

### 6. Frame skip in play

A machine behind the room (time sync says behind by more than 2 frames, or its last frame went over the frame's
time) runs the next frame's logic and sends its inputs first, and skips drawing that frame: 2D, the GPU frame,
present. Sound is never skipped. At most 1 skipped frame in a row (30 fps at worst): the logic stays at full speed,
so nobody waits. The player sees a lower frame rate instead of the whole room slowing. Shown in the FPS corner as
"FRAME SKIP". Off when the machine has spare time again for 2 s.

### 7. A player already in play who is too slow

In order: frame skip (step 6); then one board (Auto); then, if the room is still held (it waits for this machine's
inputs over 100 ms a frame, for 5 s), the move to watching as in step 4, its seat reserved, character idle, the same
notices. It takes the seat back by the same rule as a joiner.

## Network effect

- Step 2: joins judged differently. No change to what is sent, except the inputs carrying their send time, which
  they already do (TimeSync). Same build id rules.
- Step 3: one board on the joiner only: nothing changes for the others (one board is already allowed in any room).
- Step 4: a new seat state (reserved, not taken) the host and the lobby show; a protocol bump. The room never waits
  for a joiner again.
- Step 5: when a one-board joiner hands back. Same messages.
- Step 6: local only (drawing). Nothing sent changes.
- Step 7: as step 4.

## Tests (each step, before the next)

- issue22.ps1 with `-JoinSlowMs` (a joiner slower than the game), `-JoinerOne`, `-OthersOne`, and with the game held
  to 2 cores; every combination of board modes. Checked: the room's longest wait for inputs at every join (under
  0.2 s), hashes equal, the notices said on both sides.
- A new in-play slow test: `SPIKEOUT_TEST_SLOW_MS=N@FRAME` (a machine becomes slow mid-game) for step 7.
- Both local suites, the spectator tests, the internet cases, the window tests.

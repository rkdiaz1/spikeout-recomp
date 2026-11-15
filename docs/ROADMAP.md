# Roadmap

Each milestone ends with something you can run and check.

## 1. Foundation — *done*

- [x] CMake/C++20 project, CI on Windows, macOS and Linux
- [x] PowerPC 603e decoder and disassembler
- [x] CPU state struct
- [x] Model 3 Step 2.1 memory map
- [x] ROM set definition for `spikeofe` and a verifying loader
- [x] Confirm program ROM byte order against a real dump (`romcheck`): as-is
- [x] Control-flow analysis: find functions and jump tables starting from the reset vector and exception vectors (`codemap`, [notes](control-flow.md))
- [x] Lifter: emit C++ for each function: integer, branch, load/store, CR, SPRs ([notes](recompiler.md))
- [x] Lifter: floating point (13% of the game's instructions); the whole game compiles (`game_ram`) and runs into its startup (`gamerun`)
- [x] Runtime bus: dispatch loads/stores to RAM, ROM and device stubs; MPC106 PCI bridge
- [x] The recompiled boot ROM runs from reset to the jump into RAM: bridge setup, RAM clear, 1 MB copy with checksum (`boottest`)
- [x] **Goal:** the recompiled boot code runs through the ROM/RAM self-test (and on into the game's own startup and its backup RAM test)

## 2. Getting to the attract mode — *in progress*

- [x] Exceptions: `sc`, traps, and returning from a handler with `rfi` ([notes](recompiler.md#exceptions)); the game now initializes and waits for an interrupt
- [x] Interrupt controller and decrementer; the game runs frame by frame in its interrupt handlers
- [x] Banked CROM switching (bank select at `0xF0100008`, in `m3::Bus`)
- [x] 315-5881 security board decryption (key `0x29236FC8`; `m3::Crypt315_5881`, MAME's cipher, checked against it on random streams)
- [x] Open-bus reads return all ones (Spikeout's ROM check needs it)
- [x] Frame timing: 57.524 Hz, 424 lines, IRQ 0x02/0x0D as in [hardware-notes.md](hardware-notes.md) (`recomp::FrameTiming`; not yet: waiting for the IRQ 0x02 acknowledge)
- [x] Backup RAM (the game's RAM test passes)
- [x] RTC (Epson RTC-72421, `m3::Rtc72421`): on the emulated time, settable by the game
- [x] Network board: its own 68000 runs the firmware the game uploads (`m3::CommBoard`, Moira core; the simulation stays as `--netboard sim`); with the link set to master it brings the ring of one up (status 1 `0x2021`) and the game goes on to its ROM check
- [x] Security board check: passes (it showed ILLEGAL ROM before the decrypter)
- [x] JTAG scan chain of the Real3D chips; Real3D PCI ID, DMA device and ping-pong bit: the game runs its main loop and sends the Real3D real frames
- [x] Settings EEPROM (93C46 on the I/O board, `m3::Eeprom93C46`): the game writes its defaults and runs as a link master on its own; before it, it read garbage, set up as a slave and the firmware reported NO CARRIER, as a lone slave should
- [x] Task resume points: code that tasks yield to (stored `lis`/`addi`/`addic` constants) is found by the analysis; the game now runs minutes of game time without missing code
- [x] Sound board link: MIDI port and IRQ 0x40 in VBlank (the game queues its sound commands and waits on them); the bytes are kept for a sound board
- [x] Return sites: a resumed task can return into a function it called from before it yielded (`resume_at`, about 7,000 sites)
- [x] The attract mode runs: logo, title, INSERT COIN, BASIC OPERATION (16,633 frames with no missing code)
- [x] Gameplay: coin, start, fighter select and the first stage with the fighters in 3D (`gamerun --press`), 11,000 frames with no missing code. Fixed on the way: FPSCR[FPRF] after FP arithmetic (the fighters were NaN), returns past several calls (the scheduler nested until the stack overflowed)
- [x] 603e cycle counts per block (`ppc::block_cycles`: dispatch, units, register dependencies) instead of 8 cycles a block; the attract mode's title-to-demo load now takes about as long as on a real board's recording
- [ ] Reference interpreter for code that the static pass misses, with logging so it can be added to the recompiled set
- [ ] **Goal:** reaches the attract loop, verified against Supermodel's behavior

## 3. Graphics

- [x] SDL3 window and frontend (`spikeout`: the game on its own thread at 57.524 Hz, keyboard to the I/O board)
- [x] Tilegen 2D layers (`m3::TilegenRenderer`, drawn a line at a time): boot text, logos, attract screens
- [x] Real3D Pro-1000: memory, texture uploads, the viewport and culling-node walker, model decoding (`m3::Real3d`, `m3::Real3dRenderer`)
- [x] Software rasterizer (depth buffer, perspective-correct textures, sun and ambient light; bands of lines on several threads): the attract mode in 3D at about real time
- [x] Fog and the ping-pong update buffers
- [x] Matched against Supermodel frame by frame: frustum culling, LOD fades and cross-fades, back faces, the alpha-test rules (fixed holes in the fighters), opaque-before-translucent order, bilinear mipmapped textures, per-pixel lighting with specular, fixed shading and the spotlight (`gamerun --probe` for single pixels)
- [x] Native renderer (`gpu::Real3dGpu`, SDL GPU: Vulkan, Metal): the scene shared with the software renderer (`m3::Real3dScene`), shaders that sample, light and fog as it does, the same passes; the window draws the 3D with it, and `gamerun --renderer compare` checks it against the software picture (through a gameplay run, about 0.1% of pixels differ, along polygon edges)
- [x] GPU renderer: quads drawn whole (`--quads`), as in software
- [ ] GPU renderer: the probe, decals in the translucency layers seeing the opaque pass's; Direct3D 12
- [x] High resolution and antialiasing: `spikeout --render-scale N` draws the 3D N times as large; shown in a smaller window it is averaged down through mipmaps (supersampling)
- [x] Widescreen (Display, Picture shape: the window's, 16:10, 16:9, 21:9): full-screen viewports opened out as Supermodel does, the 2D in the middle and its sides filled from the scrolling layers' name tables, plain edges and repeating textures (`m3::WideLayer`)
- [ ] **Goal:** attract mode and gameplay render correctly

## 4. Sound and input

- [x] 68000 sound CPU + two SCSPs (`m3::SoundBoard`, `m3::Scsp`, adapted from Supermodel's SCSP with fixes from the chip's register layout and MAME's board map): the sound effects play in the window (SDL audio) and `gamerun --wav` records them
- [x] MIDI link timed by the board's serial chip (`m3::Usart8251`, 31,250 baud): IRQ 0x40 is its TxRDY, each byte reaches the sound board 320 µs after the last (Supermodel's burst stays as `--midi-burst`)
- [x] DSB2 MPEG music board (`m3::Dsb2`: its 68000, the serial line at 31,250 baud, the decoder's commands, MPEG Layer II through minimp3): the music plays
- [x] Supermodel's mix: music with SCSP 1 at the front, SCSP 2 at the rear, averaged to stereo; separate sound and music volumes (`--sound-volume`, `--music-volume`)
- [x] Settings menu in `spikeout` (`src/ui/`, RmlUi over the paused game, after vf3-recomp's): display, sound and music volume, controls, game; kept in `settings.ini`
- [x] Show FPS, overall volume; the arcade's settings (difficulty, free play, demo sound: `tools/arcade_settings.hpp`, the EEPROM's layout and checksum); EEPROM and backup RAM kept between runs
- [x] Region (Japan, U.S.A., export, Australia): the game's own setting in the EEPROM, which its operator test menu shows but skips
- [x] Speaker setup in the settings menu: stereo, or the cabinet's front and rear pairs apart (one output of four channels, or two outputs)
- [x] CRT colours (Supermodel's) and a screen filter (scanlines, CRT; after vf3-recomp's)
- [x] Keyboard (two players, rebindable in the menu) and gamepads (fixed layout)
- [ ] Rebindable pad buttons; arcade stick mapping

## 5. Local multiplayer

Spikeout is one player per cabinet; multiplayer means linked boards.

- [ ] Network board emulation (Supermodel `Src/Network/` as reference)
- [ ] Run 2–4 boards in one process with a virtual link → local co-op on one PC

## 6. Save states

- [x] Complete serializable machine state per board (CPU, RAM, video, sound, I/O, network board): `m3::State`, each device's `state()`, `tools::board_state`; resumed at any basic block (the lifter's `*_blocks()` tables, `Context::resume_block`). `gamerun --save-state FILE@FRAME` / `--load-state FILE`; the window's F9 / F10 / `--load-state`
- [x] Determinism: a run loaded from a state is the same to the byte as the one that saved it (RAM hashes, sound, screenshots and the next state all equal)
- [x] Record and replay: `spikeout --record FILE`, `gamerun --replay FILE` (inputs every frame, RAM hashes to check)
- [ ] Save/load in under a millisecond (a state is 29 MB, mostly Real3D memory: keep what changed, compress)
- [ ] Linked boards (`LinkRing`) in a state; a recording that starts from a state
- [x] F10 after a restart: the newest save in the saves folder (0.1.0-preview5.1, issue 27)
- [x] **Release 6 (done 2026-11-15; the header's version raised to 3, not kStateFormat, which is the state's own layout and unchanged; "time played" is the game's frames since its start; the cabinet's difficulty shown too; a scroll bar on long pages):** F10 opens a list of saves to choose from, newest first (a page of the Esc menu; keyboard or pad). Each row: a small picture of the moment (F9 saves one beside the state), the stage (read from the game's memory when saving), time played, the date. Saves from another version marked and confirmed, damaged ones greyed out; the version and checksum checks as now; loading keeps the game left (F9 within 5 s goes back); delete asks first; not in network play. The state header gains the stage and time played (raise `kStateFormat`; older saves still load, shown with "—")
- [ ] **Safe loading** (states come from files and from other players' computers in public rooms: a bad one must be refused, never read or write outside memory). Pull request 26 (2026-11-14) checks the 3D, SCSP, music board and EEPROM fields after loading, and fixes the 3D update buffer's endless walk and the sibling chain's recursion; to merge after its CI and our picture-hash proofs. On top of it, most useful first:
  - [x] A size limit on what a peer's packed state or other packet may unpack to (release 6: `net::decompress` takes a cap; every board's state 160 MB, one board's 48 MB, other packets 4 MB)
  - [x] States taken only when asked for and from the player expected to send them (release 6: every board's state only by a joiner waiting for it or a machine that asked to be put back in step, from the host or the authority; a board's state only for the seat and frame asked for, or a leaving player's announced frame; backups are each player's own board's, from that player)
  - [ ] A fuzz test: a real state with random bytes changed, loaded and run a few frames under AddressSanitizer and UBSan (Linux); finds what reading the code misses, across the whole state. Device-only states in CI (no ROM there), whole boards on this PC. (Release 6: the devices' part, tests/test_state_load.cpp, SCSP, music board, EEPROM, JTAG and the decryption chip, `SPIKEOUT_FUZZ=N` rounds, `SPIKEOUT_TEST=NAME` runs one case; clean under AddressSanitizer and UBSan at 3,000 rounds a device, 30,000 without. It found: the music board's and the sound board's sample count far from their 68000's clock ran the CPU billions of cycles (the game stood: now both checked against the clock), the music board's output samples overflowing, and a bool loaded as 128 (`State` now reads every bool as 0 or 1, saved bytes unchanged). Whole boards still to do)
  - [x] The devices pull request 26 leaves (release 6): the decryption chip's buffer places and bit numbers, JTAG's state and register lengths, the simulated network board's state checked when loaded; the network board's forwarding kept inside its frame. Looked at and safe as they are: the tile generator and `TileSides` (every read masked or size-checked; its registers any value the game itself can write), the 68000s and the PowerPC (registers only; memory reads by address checks), the serial ports, the RTC, the bus's ROM banks, the link ring (a state's lists never longer than its bytes)
  - [x] ~~The SCSP's LFO switched on while a slot plays reads a null table in live play~~: not so here (checked 2026-11-15): every write of that register sets the LFO's tables up (`update_slot_reg`, 0x12, as Supermodel); only a loaded state could leave them unset, and pull request 26's check refuses that
  - [ ] State tests that find fields by name (a map of each field's place kept by `m3::State`), not by byte offsets that break whenever `kStateFormat` changes

## 7. Online play

See [netplay.md](netplay.md).

- [ ] Mode A: one board per PC, cabinet link over the network (LAN first)
- [ ] Mode B: rollback, every PC simulates all linked boards; 2–4 players plus spectators
- [ ] Lobby / matchmaking; Steam networking as an optional backend
- [ ] **One board a PC, open problems** (found 2026-11-14; rare: 9 of 1,251 network problem reports were on one board; details in [netplay.md](netplay.md), "Joining a long game"):
  - A player joining an empty seat in play never gets the game: nobody runs an empty cabinet whole, so the host asks the joiner itself for it. Cure: the host (the authority) runs empty cabinets whole and answers for them.
  - A player leaving on a very slow upload (under ~0.5 MB/s): its final state (~5.4 MB) isn't through in the 10 s it waits, so the others never have its cabinet whole and a later joiner of that seat can't get in. Cure: the others fall back on its last backup (every 120 frames) and run it on with its inputs, as for a player gone without a goodbye; exact, at worst a short hitch (to measure).
  - An empty seat keeps one board from ever handing back to side runs.
  - A one-board joiner's hand-back to side runs while catching up can go out of step (made packets differ from the real ones); waits for the decompile.
  - Once, an all-one-board room stood at the frame a one-board joiner's cabinet began; not found yet.
- [x] **Release 6 (done 2026-11-15; the pill's colours to pick from a preview):** the room's difficulty, shown before joining and while playing: in the lobby's room details and join list, and in the network status's room line (`HOST NAME · ROOM CODE · HARD`), as a coloured pill (suggested: the easiest green, normal white/grey, harder orange, the hardest red; the colours picked from a preview). Linked play uses the master cabinet's setting, so only the host's is needed. Its EEPROM word: see `tools::ArcadeSettings` (tests/test_devices.cpp, "arcade settings"); the level names as the game's test menu says them. The lobby's new room field optional, so older builds still list and join

## 8. Release

- [ ] Packaged builds for Windows, macOS (universal), Linux and Steam Deck
- [ ] First-run ROM import wizard

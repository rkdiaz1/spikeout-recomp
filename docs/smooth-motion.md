# Smooth motion: the plan

Status (2026-11-21): steps 1-3 built, step 4 but the VRR multiple and present times. Smooth motion is off by default:
the menu's Display > Smooth motion (or `--smooth`); the old guessing version, `m3::interpolate`
(src/runtime/real3d_interp.cpp), is kept for comparison (SPIKEOUT_SMOOTH_GUESS=1). This page is the design that replaces it,
agreed with the owner. Anything that comes from the game's code is read at runtime from the player's own ROM.
Nothing made from it ships, and the private decompile notes that point the way are never committed.

## The aim

Show the motion in high refresh rates without ghosts, and never change the game:

- The game keeps running at the arcade's 57.524 Hz. It moves everything a fixed step each frame, so running it at
  the monitor's rate would make it faster: 4.3% at 60 Hz, 2.5x at 144 Hz. Network play also needs every PC at the
  same speed.
- Only the drawing changes. Each refresh of the monitor gets its own picture, placed between the last two game
  frames. Sync, rollback, save states, replays and hashes are untouched. **Network effect: none.**
- With smooth motion off, the output stays exactly as it is today, proved by GPU picture hashes on replays.
- It stays **off by default**. To draw a picture between frames N and N+1, the game must already have made N+1, so
  smooth motion adds up to one game frame (~17 ms) of delay. The menu says so. (Issue 7 already reports the controls
  feeling floaty, so the default must not add delay.)

## Why today's version ghosts

`m3::interpolate` only sees the Real3D scene. It guesses which model in one frame is which model in the next (by
viewport, polygon address and culling-node path, else the nearest), and blends each model's whole eye-space matrix
on its own. When a guess is wrong, or a model is a flipbook (a different model each frame), it ghosts. Seen at: the
tower light, car wheels, the gate, the sea birds, the sea level.

## The design: three layers

**1. The camera, from the game's own view.** The game has one camera for all its 3D. Its view matrix goes into the
Real3D's matrix table, and each 3D viewport's first culling node names it (as on other Model 3 games: the camera is
the viewport's root matrix). Blend it, position straight and rotation along the shortest turn, and draw everything
else in world space through it. Cuts come from the game's own camera state, read from RAM after each frame, not
guessed. Measured over 150,000 frames of the whole game, every jump was one of:
- an area or stage change (the camera's driver changes);
- a scripted shot starting a new instant command (a set position, a path's first key, the ending's shots);
- a win pose's camera taking over, and each of its shots starting.

Nothing else moved the camera more than 0.6 units a frame. A scripted command can move the look-at point along its
line (2 units at once) without changing the view, so only the eye and the view's turn are tested. The camera shake
(a vertical bob that changes direction every frame or two) is taken out before blending and added back unblended;
blended, it would be halved. The game keeps the unshaken eye beside the shaken one, and the shake only lifts the eye,
so it's exact. The field of view is fixed apart from a few modes, so it's taken from the newer frame.

The 3D drawn usually holds the camera of the frame it was made in, but in some places (seen after an area change,
for the rest of that area) the frame before's. The camera is kept for both frames and the blend takes the one the
viewports hold.

**2. The characters, bone by bone.** Each character's bone matrices are in RAM every frame, in world space. Match
them by character slot and bone number, no guessing; the Real3D's matrix numbers for them are handed out in draw order
each frame, so a scene node is tied to its bone through that order. Blend rotations as rotations, not raw matrices, so
fast swings don't shrink limbs. Head and hand changes are model swaps on the same bone matrix: the newer model on the
blended matrix. A character is placed (not moved) only when its slot is created, a new player linking in included:
those two frames aren't blended. Mirrored motions are mirrored before the skeleton is built, so nothing to handle.

**3. Every other model: one rule for all.** Measured on frame pairs of every stage the game uses (the coast road and
docks, Diesel Town, the shipyard and ship, the opera house), and checked against every routine that draws a model,
whatever drives each model, it falls in one of:
- **moved, same model** (birds, boats, traffic, the trailer truck, stage 1's gate and the stages' other gates and
  machines, swinging and sliding doors, pick-ups and weapons thrown or falling, an enemy car rocking): the same
  model address within 3 units in the frame before. Blend it.
- **spinning** (helicopter rotors, propellers, the lighthouse beam, a pendulum, the merry-go-round, a car's cover
  flying off): a turn of more than ~10 degrees a frame with the centre barely moving. Blend the angle, never the
  matrix (a blended matrix shrinks a spin).
- **flipbook** (the sea surface, the water round Phoenix's deck, the opera's curtain opening, neon and TV screens,
  house windows): a different model of the same animation at the same place. Draw the newer, never blend. This is
  where the "sea level" ghost came from.
- **flipbook that moves** (the gulls, whose wings are 16 models, and effects that drift): a model of the same
  animation within 3 units. Blend the place, draw the newer model.
- **many identical copies** (rain streaks, splash rings, a crowd of one effect): nearest matching mixes them up, so
  draw the newer only.
- **shadows:** each follows its character (it sits at the character's floor point), not matched on its own;
  identical shadows of different characters would be mixed up.
- **appeared or vanished:** draw the newer only.
- **the radar map and the HUD's 3D** (the charge lamps and gauge): their own viewports, fixed to the screen. Leave
  them unblended.

"The same animation" comes from the models' names in the player's ROM, read at start (the name without its
trailing number: a gull's wing models, numbered 01 to 16, are one animation; names ending _300/_500/_700 are detail levels of one model,
not animation). Always with the same-place test: the same name at different places is separate objects (a broken
door's pieces). Nothing made from the names is shipped.

The stage's model tree is double-buffered (drawn under one viewport one frame and its twin the next), so matching
goes by model address and world position, across viewports, not by viewport or node path.

Lighting, fog, textures, level of detail and the 2D layers stay the newer frame's, as now.

**Pairing frames:** a scene built at frame N holds the camera as RAM has it after frame N, and the bones as RAM had
them after frame N - 1. The renderer reads each from the right frame. The game has no hit pauses or held frames (every
body never stands still at once), so blending can't drift.

### Later: true in-between poses

The game's motions are cubic Hermite curves, which we can already evaluate exactly. A character's pose could be
computed at frame 21.5 rather than blended, which is real 120 Hz animation for the limbs. It isn't first because:

- only the limbs would be "real": the character's place in the world, the camera, hits and props still step at
  57.5 Hz and are blended anyway;
- halfway between two frames, a smooth curve and a blend land almost in the same place; the gain shows only at sharp
  snaps, like the end of a punch;
- the pose also depends on the motion cross-fades, the feet fitted to the floor and the hit pauses, so it means
  running the game's drawing-side code for half frames.

It builds on layer 2 (the same bone matching), so it stays open as an upgrade if a difference still shows.

## Pacing: one rule

**Each refresh, show the picture for the moment it appears on screen.** The blend weight is how far that moment falls
between the two game frames.

- **Smooth motion off:** the weight is always 0, so the arcade's 57.524 Hz as now.
- **VRR screen** (G-Sync, FreeSync): set the refresh to the highest multiple of 57.524 Hz the screen allows (115 Hz
  on a 120-165 Hz screen, 230 Hz on 240 Hz). Every step is then the same size, so small timing errors can't show.
- **Fixed screen** (60, 99.9, 144 Hz): the monitor's own rate. The steps differ in size, but everything is drawn where
  it should be at that moment, so the motion looks even.

Without smooth motion on a fixed 60 Hz screen, one picture shows twice about every 0.4 s. VRR, or a custom 57.5 Hz
mode set by the player (NVIDIA/AMD control panel, CRU), removes that; the game can't.

The pacer (tools/spikeout.cpp) runs an even clock a frame behind the game, a picture each refresh of the screen (it
was capped at 120 a second until 2026-11-21).
Its even pacing stays: the display clock moves one refresh a picture, pulled a tenth of the way to the real clock each
time (a jump to it after a hitch of more than two refreshes), so the in-between steps are the same size even when a
picture starts a little early or late. To change: the cap (the setting below), picking the VRR multiple, and the
weight from the time each picture is actually presented.

### No frame limit setting

Smooth motion on draws a picture each refresh of the screen (144 on a 144 Hz screen, 240 on 240 Hz; on VRR, the
multiple of 57.524 Hz above); off, the arcade's 57.5 a second. Only the pictures change: the game always runs its
57.524 frames a second, so its speed and network play are never affected. The on/off switch is the only setting
(owner, 2026-11-20).

Left out on purpose:
- Fixed numbers (60, 120): a cap that isn't a whole part of the screen's rate (120 on a 144 Hz screen) shows some
  pictures for one refresh and some for two, the uneven motion smooth motion is there to remove.
- Half the screen's rate: on 60-120 Hz screens (most PCs, handhelds) it gives 30-60 pictures a second, no better
  than smooth motion off; only a 240 Hz screen would gain from it. A weak or hot device turns smooth motion off.
- None: more pictures than refreshes are never seen (tests can lift the cap with an environment variable).
- Anything that lowers it on its own (vf3-recomp's governor held fast machines at 60, so it was turned off there).

To watch: at 240 Hz a picture has 4.2 ms; a high render scale may miss refreshes there. If so, revisit (half the
rate on such screens only).

## Steps

1. DONE. The camera: blended from the viewports' camera matrix, with the game's cuts and the shake taken out. Everything
   else drawn in world space at the newer frame. Check the ghost scenes with replays.
   BUILT (2026-11-20, branch claude/smooth-camera): m3::interpolate_camera (src/runtime/real3d_interp.cpp), the
   game's camera read by games/spikeofe_camera.cpp after each frame, the default in the window (SPIKEOUT_SMOOTH_GUESS=1:
   the old matching) and in gamerun --interp (--interp-guess: the old). Checked: each 3D viewport's first node holds
   the game's view matrix bit for bit in the same frame (rotation transposed); the radar's and the HUD's viewports
   have their own (identity) and are left as they are; the docks, Phoenix's deck, the coast road and the helicopter
   drawn halfway as expected (gamerun --interp --renderer gpu --gpu-scale 1 --screenshot X --shot-every 1; the
   halfway writer needs scale 1). Then the same day: the shake taken out and added back (the camera's unshaken eye,
   kept by the game), cuts on the eye (over 1 unit), the view's turn (over 20 degrees) and a win pose's new shot; the
   look-at no longer tested; the camera of the frame before used when the viewports hold that. Checked: stage 1's
   3,400 frames, two shakes included, blended throughout with no cut (the eye moved at most 0.15 a frame, the view
   turned at most 2 degrees); the bot's seg3 replayed with its inputs (4,080 frames): every jump in the decompile's
   list cut (an area change, two win-pose shots, the stage's end and the next start; each jump's angles change a
   frame later, cut by the turn, 70-180 degrees), the frames after the area change drawn a frame behind blended.
   Smooth off: the docks' 1,000 frames and stage 1's 600 hash the same as main.
2. The other models by the one rule (moved, spinning, flipbook, appeared, radar).
   FIRST PART BUILT (2026-11-21): interpolate_camera's `models` (the default; gamerun --interp-camera-only and the
   window's SPIKEOUT_SMOOTH_CAMERA_ONLY=1 leave it out). Each model's place in the world (its viewport's camera
   undone) is found in the frame before by its polygon address, in any viewport with the game's camera: the nearest
   within 3 units, with no other copy within twice its distance; found, the place is blended (position straight,
   the shortest turn: spins keep their size; more than 120 degrees: not blended). Characters' parts are models too,
   so until step 3 each limb is blended by its own place, which is what the bones would give. Not found (appeared,
   a flipbook's next model, moved too far) or copies alike: as in the newer frame. Measured on the ghost scenes
   (frame pairs): the docks ~105 still, ~67 blended, 2-4 newer; Phoenix's deck ~75 still, ~170 blended, 7-14
   newer, 6 alike; the coast road ~335 still, ~150 blended; the helicopter ~100 still, ~245 blended, 10-16 newer,
   5-11 alike. Halfway pictures: the characters fighting whole, limbs between, no seams at the joints. The blend
   costs 0.64 ms a picture on average (0.33 for the camera only), at most ~1.9 either way; the window's 100
   pictures a second all blended, the game at 57.52, stutters seen as often with the camera only (this PC).
   SECOND PART BUILT (2026-11-21): the moving flipbooks. games/spikeofe_models reads every model's name from the
   player's program ROM at start (8,789 names) and groups the models of one animation (a name but for its trailing
   number; the detail levels _300/_500/_700 left out: 173 animations); each frame it reads the RAM table of where
   each model starts (rebuilt only when it changes, at loads), the scene builder gives each model the number of
   the nearest such node above it (Real3dSceneBuilder::set_model_nodes, Real3dScene::Model::number), and the game
   code its animation. A model not found by its address is found as the nearest of its animation's models (same
   rules): its place blended, its own model drawn. Measured: Phoenix's deck 4-6 a frame (the water round it), the
   helicopter 12-14 (the gulls in the sky); halfway, each gull once, between its two places, the rotor blades
   between their angles. Nothing made from the names is kept or shipped; no name is written in our code. Rain and
   splashes stay with "copies alike" (naming them would put the ROM's names in the code). Smooth off: the docks'
   and stage 1's hashes the same as main; the window at the helicopter: every picture blended, no stutters.
   THIRD PART (2026-11-21, from the decompile's joint-skin and respawn findings):
   - Skins: the game builds each joint's skin in polygon RAM every frame, its address packed by whoever draws that
     frame, so a skin matched by address could be blended from another character's joint. Skins aren't matched now:
     each moves as the limb on its bone (drawn with the same turn, bit for bit: the skin sits 0.01-0.2 units off the
     limb's point, and any point fixed to the bone moves as the limb does); no such limb (its bone's own model left
     out at a lower detail), as the nearest model within 1 unit. Stage 1 3500-9000: every skin placed so, 2 a frame
     by the nearest in ~40 frames.
   - Bodies put down in one jump (made, the lift 2 units the frame after, put back on the ground, snapped back):
     games/spikeofe_camera reads the 20 bodies' records each frame (Real3dScene::placed: those of this frame and the
     one before, as the 3D draws bodies a frame late); a model within 2.5 units of one is drawn as in the newer frame.
     A body is placed when its record was empty, its respawn byte jumps to 216, or it moved more than 1.5 units
     (the decompile's fastest move 0.95). Word 0's bit 0x40000000 isn't used: it stays set on live enemies in a
     fight. Seg3 replayed with its inputs: the enemy made at 63581 and lifted at 63582 found (the decompile's list).
   - The matching done once for each pair of frames (m3::CameraBlend: prepare, then draw for each picture); the
     window keeps the pair and its version. Gamerun's interpolate_camera is the two in one.
   Checked: seg4's ending (189691-190000): the four shots cut (two frames after the decompile's numbers, the same
   for all four: the runs count frames differently); stage 1's gate area, a fight halfway: whole, between; smooth off
   hashes as main; the window on Phoenix's deck: every picture blended at 100 and at 144, the 3D 0.8-1.7 ms.
   Still to do in step 2: the gates, machines, curtain and traffic seen moving in a replay.
   STAGE 4 ("DEPART", reached by the bot's seg4 replay at 103,056-152,340): four stretches of ~290 frames blended
   with no cut but the first frame after loading a state; 0.1-4.6 models a frame as in the newer frame; the record
   shop (128197: the TV screens and neon signs on screen) whole halfway. The merry-go-round isn't on screen anywhere
   in the bot's run of the stage (every sixth state looked at): not checked.
3. The characters' bones.
   BUILT (2026-11-21), without a hook in the recompiled code: games/spikeofe_camera reads the live bodies' bone
   matrices each frame (the 20 records, +860: the root then the 19 drawn, 48 bytes each, the game's layout: the turn
   column by column, then the place); a scene keeps both the last read and the one before (Real3dScene::bodies,
   bodies_before). A model whose turn is a bone's (each entry within 0.001) and whose place is within 1 unit of it
   is keyed by (body, bone) and found in the frame before by that key, no address or nearest guessing. Measured: the
   3D draws the bones of the read before (103 of 103 limbs on Phoenix's deck match that read, almost none the
   current one, as the decompile said). Tied a frame: the deck ~104, the helicopter ~140, the gate ~60, seg3's crowd
   ~148. Halfway: four fighters close together (seg3 63651) each between its own two poses. The "copies alike"
   left (~15 a frame in seg3) are effects lying on the ground at the fighters' feet (one model, many copies; they
   never move, so the newer frame is exact). Smooth off hashes as main; the window: 99-100 of 100 pictures blended.
   FIXED THE SAME DAY (the owner saw it in the halfway picture of seg3 63651): White's head showed its face side,
   his back's lettering was turned and a shin was missing, and some enemies were broken the same way. A bone can
   carry more than one model, and the key was (body, bone) only: the second model was blended from the first one's
   place. The key is now (body, bone, the model's address); a model swapped on its bone (a head's or hand's) is
   found by the bone alone only when the frame before had one model there. Halfway pictures of seg3's crowd, the
   deck and the helicopter fights: every fighter whole.
   SHADOWS (corrects the line above: those "copies alike" at the fighters' feet were the shadows): the game draws
   each body's shadow as many 5-polygon models, flat on the ground, in a ring up to ~1.4 units round its feet. A
   flat model (its up axis within 0.99 of straight up) not found or alike, within 1.5 units of a body's bone across
   the ground, moves with that bone across the ground from the frame before (the same body and bone), its height
   its own. A still floor model (a manhole) is found as still first, so never moved. Seg3: "alike" 14.8 a frame ->
   2.2, shadows 13.2 a frame moved with their bones; halfway, each body's shadow under it.
   AFTER THE OWNER'S FIRST WINDOW TEST (2026-11-21; screenshots of frames 31880-32539, a state at 20745):
   - Characters still broke now and then. Two causes: (1) a straight joint gives two bones the same turn, so a
     limb's bone could differ between the frames, and the fallback "the bone alone" paired it with another part's
     model; the fallback is now (body, the model's address), used only when that model is on the body once. (2) Each
     limb was blended on its own, turning the shortest way about its own point and its place in a straight line: a
     limb swinging 60-90 degrees in a frame (a punch: seg3 has such turns nearly every frame) came apart from its
     neighbours at the joints. Now THE SKELETON is blended: each body drawn in both frames has its 20 bones' turns
     and places taken relative to their parents (the chain from the RAM's bone order: the root; chest from the root,
     head, armpits, shoulders, arms, hands from the chest; waist from the root, thighs, knees, ankles, toes from the
     waist), blended there, and the chain rebuilt from the root; each limb and skin moves with its bone
     (Kind::bone). Joints stay joined however fast a limb swings. The parent table is a reading of the draw order;
     a wrong parent would only change the path between the two frames, never the ends.
   - Skins' shapes: paired with the frame before's skin on the same body and bone (in drawing order, the same
     polygon layout), each vertex goes part of the way back to where the older skin had it, in the skin's own
     frame (the decompile's "simple" way): a skirt between the waist and both thighs bends with the legs (a
     camo-jacketed enemy's flared trousers were torn in the owner's screenshots). 10 of 10 skins shaped on the
     deck and stage 1.
   - The owner's state (20745, a fight by the fence) replayed: halfway pictures whole, the character knocked
     into the air between its two poses with no gaps; the window from it: 99-100 of 100 pictures blended, the 3D
     2.5-2.8 ms a picture, the game at 57.52 with no stutters.
   - The van's wheels changing texture as the camera moves (stage 1's car park, seen from the docks side): the
     owner saw it with smooth motion off too. Checked: Supermodel shows the same, the game's own look
     (docs/fidelity.md "Checked: the parked van's wheels").
4. The pacer: a picture each refresh, the VRR multiple, the weights from present times.
   FIRST PART DONE (2026-11-21): a picture each refresh, whatever the rate (the 120 cap gone; SPIKEOUT_SMOOTH_HZ=N
   forces N for tests). Measured on the 99.95 Hz screen: 100 a second, each 0.576 of a frame on (spread 0.02); forced
   to 144: 144 a second, 0.400 on (spread 0.02), the 3D 1.6-2.0 ms a picture, the game at 57.52 with no stutters.
   Left: the VRR multiple (SDL doesn't say whether a screen is running VRR; needs a VRR screen to try. WAITS FOR A
   TESTER, the owner's decision 2026-11-21: on the README's roadmap with a call for VRR owners. Then: a "VRR screen"
   setting; smooth off, each frame presented as it's finished (the screen at 57.524); smooth on, the highest multiple
   of 57.524 the screen allows (115, 230; half rate halves it to a multiple too); whether drivers apply VRR to a
   borderless window with mailbox presenting or only with plain vsync (FIFO gave 33 a second on the owner's PC: not
   for everyone); the tester's window log (pictures a second, the clock's spread) to check it), and the
   weights from present times (SDL's GPU API doesn't say when a picture reached the screen; the even clock keeps the
   steps within 0.02 of a frame meanwhile). A 240 Hz screen to check the 4.2 ms a picture: none here.
   240 A SECOND, MEASURED (2026-11-21, forced with SPIKEOUT_SMOOTH_HZ=240 on the 99.95 Hz screen, Phoenix's deck
   fight from bot-22700, SPIKEOUT_SMOOTH_LOG): before the fight 240 a second; in it 197-224 (144 holds exactly).
   Not the GPU: render scale 4, 2 and 1 alike. The CPU per picture: the 3D 2.0-2.4 ms (of it, matching a new pair
   0.4 on average, blending 0.5: the log now splits them; the rest building the 3D for the GPU, ~1.1), presenting
   0.5-0.6. Within the 4.2 ms on average, but the picture where a new frame arrives (the matching, ~1.7 ms, falls in
   it) runs late, so some refreshes get none. To hold 240: the matching done off the window's thread (on the game's,
   or a worker, as each frame finishes), and the 3D's build made cheaper.
   DONE THE SAME DAY: THE MATCHING ON ITS OWN THREAD (tools/spikeout.cpp SmoothMatcher; the owner chose a thread of
   its own over the game's, so the game's thread and network play stay untouched). The window hands each new frame's
   3D scene over (one copy, as it kept the frame before itself); the thread keeps the frame before, matches the pair
   and publishes both frames with their match; a picture blends from the newest pair that is its two frames. Its
   match not ready yet (just after a new frame): the frame before, which the last pair holds (the picture's place is
   near it then), not a jump to the newer. Measured as above: forced 240, 237-241 a second through the fight (was
   197-224); a picture's 3D 0.7-1.6 ms (was 2.0-2.4); the matching 0.5-1.8 ms a frame on its thread; 0-8 pictures a
   second the frame before. At the screen's 100: 100 a second, 0-1 the frame before. Smooth off: the window
   unchanged (57.5 shown a second, nothing of smooth motion run); gamerun not touched.
   AND HALF RATE WHEN IT CAN'T KEEP UP (the owner's choice over a frame limit setting or vf3-recomp's 60): a picture
   more than 1.5 refreshes after the last is a refresh missed; over 5% of them two seconds running, on a display of
   119 Hz or more, the pictures go at half the rate (each shown two refreshes: even), full again after 10 s, the wait
   doubled (to 160 s) each time it halves again within a minute of trying. The window's log says each change.
   Checked with forced 240: halved, full again after 10 s, ran 240 with no misses for 30 s, halved again with a 20 s
   wait. Not checkable here: whether a real 240 Hz screen misses refreshes. Forcing a rate past this 100 Hz screen's
   holds some presents up (misses 7 a second at 120, ~16 at 144, ~28 at 200, 57 at 240, whatever the load; 0-2 at
   its own 100), so forced runs halve at once; SPIKEOUT_SMOOTH_FULL=1 (tests) never halves. Waits on a player with
   such a screen.
   PRESENT-TIME WEIGHTS, TRIED AND SET ASIDE (2026-11-21): the window's own clock is free-running, not tied to the
   display's refreshes; with mailbox presenting a picture near a refresh boundary can make one refresh get two (one
   never seen) and the next none. Measured by when each present returned, against the vblank times Windows' desktop
   compositor gives (DwmGetCompositionTimingInfo, no SDL change; its refresh count cRefresh doesn't move each refresh,
   the vblank time does): 0-13 refreshes a second in the fight. Tried: each picture timed to just before the
   display's next refresh, weighted for that refresh's time. Worse, and not measurable: SDL's present can itself wait
   for the display (acquiring a swapchain image), so when the present call returns says nothing about when the
   picture reached the screen; 9-35 pictures a second looked late, and one run fell to 63 a second. Doing it
   properly needs the real present times: Vulkan's present-wait or DXGI's frame statistics, both below SDL's GPU API
   (its swapchain isn't reachable), or checking the screen itself (Desktop Duplication: each composed picture, to
   count repeats). Reverted; the free clock stays.
   THE MENU (2026-11-21): Display > Smooth motion, off by default, kept in settings.ini (smooth_motion); its
   description says it adds up to one frame (17 ms) of delay and needs the GPU renderer. spikeout --smooth still
   turns it on (and, as --quads, it's saved if the settings are). Checked: a portable copy with smooth_motion = 1
   in its settings.ini, started without --smooth: 100 pictures a second, blended.
5. Maybe: true in-between poses.

Each step: smooth off proved unchanged by hashes; replays of the ghost scenes (the coast road's car park, the docks
with the sea, birds and lighthouse, Phoenix's deck, a helicopter, stage 1's gate) compared before and after.

## What we still need to know

The decompile answered the camera, the characters, every animated model and the timing (2026-11-20). A second
round (the same day) placed every routine that draws a model, stage 1's gate included. A third gave the shake, the
scripted shots, how the renderer can name each model from the ROM at run time, and how to tie a character's scene
nodes to its bones (a read-only hook on the game's bone drawing: it changes no game state, but it is part of the
recompiled code and changes its fingerprint; what that means for save states and network play between versions is
to check before it's added). Found with it: before its bones, a character is
drawn with a few models the game builds in polygon RAM each frame (likely the joints' skin): step 3 must pair their
vertices. Left:
- DONE (2026-11-21): a new player linking in, two boards (gamerun --boards 2 --parallel, the linking screen's
  repro in CLAUDE.md, a state saved at 4500): play resumes at 4540 with player 2 beside player 1 on board 0, and
  every halfway picture has each fighter between his own two poses, nothing doubled or missing; the linking screen
  (2D) unchanged. Seen in the frames themselves, smooth motion off as on: the new player drawn dark in some frames
  (4541, 4546-4547, 4550-4551), likely his flashing after the join; Supermodel draws the same from our dump of 4541;
- the gates: stage 1's first gate replayed (fullrec/bot-6818 with stage1/inputs.bin, 6819-7030): the camera's
  shakes as White hits it blend cleanly, but the gate itself doesn't move as it breaks (the area just clears). The
  owner checked a gate moving in the window with smooth motion on: fine (2026-11-21);
- LEFT AS IS (the owner's choice, 2026-11-21: expected to work, not worth a dedicated check): the items below.
- the moving traffic seen in play (only checked through the developer menu's stage test);
- the gates, machines and the opera's curtain seen moving in a frame pair (read from the code; the pairs taken
  caught them still);
- true in-between poses (step 5): not started, not needed before it.

## Credits

The blending and pacing ideas come from vf3-recomp (MIT, mr-finster: src/video/r3d_scene.h) and Model 3-Vulkan
(camera-first motion, VRR multiples of the arcade's rate).

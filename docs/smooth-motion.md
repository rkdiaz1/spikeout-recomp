# Smooth motion: the plan

Status (2026-11-20): step 1 (the camera) built, the rest planned. Smooth motion is off by default and only on
with `--smooth`; the old guessing version, `m3::interpolate` (src/runtime/real3d_interp.cpp), is kept for comparison. This page is the design that replaces it,
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

Nothing else moved the camera more than 0.6 units a frame. The camera shake (a vertical bob that changes direction
every frame or two) is taken out before blending and added back unblended; blended, it would be halved. The field of
view is fixed apart from a few modes, so it's taken from the newer frame.

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

Today's pacer (tools/spikeout.cpp) already runs an even clock a frame behind the game, capped at 120 pictures a second.
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

1. The camera: blended from the viewports' camera matrix, with the game's cuts and the shake taken out. Everything
   else drawn in world space at the newer frame. Check the ghost scenes with replays.
   BUILT (2026-11-20, branch claude/smooth-camera): m3::interpolate_camera (src/runtime/real3d_interp.cpp), the
   game's camera read by games/spikeofe_camera.cpp after each frame, the default in the window (SPIKEOUT_SMOOTH_GUESS=1:
   the old matching) and in gamerun --interp (--interp-guess: the old). Checked: each 3D viewport's first node holds
   the game's view matrix bit for bit in the same frame (rotation transposed); the radar's and the HUD's viewports
   have their own (identity) and are left as they are; the docks, Phoenix's deck, the coast road and the helicopter
   drawn halfway as expected (gamerun --interp --renderer gpu --gpu-scale 1 --screenshot X --shot-every 1; the
   halfway writer needs scale 1). Not yet: the shake taken out and added back. While the camera shakes (10-75
   frames at a hit), nothing is blended; over 3,400 frames of stage 1 those were the only cuts, no false ones.
2. The other models by the one rule (moved, spinning, flipbook, appeared, radar).
3. The characters' bones.
4. The pacer: a picture each refresh, the VRR multiple, the weights from present times.
5. Maybe: true in-between poses.

Each step: smooth off proved unchanged by hashes; replays of the ghost scenes (the coast road's car park, the docks
with the sea, birds and lighthouse, Phoenix's deck, a helicopter, stage 1's gate) compared before and after.

## What we still need to know

The decompile answered the camera, the characters, every animated model and the timing (2026-11-20). Left:
A second round (the same day) placed every routine that draws a model, stage 1's gate included. Left:
- a new player linking in, confirmed with a two-board run (expected: a slot created, like any other);
- the moving traffic seen in play (only checked through the developer menu's stage test);
- the gates, machines and the opera's curtain seen moving in a frame pair (read from the code; the pairs taken
  caught them still);
- true in-between poses (step 5): not started, not needed before it.

## Credits

The blending and pacing ideas come from vf3-recomp (MIT, mr-finster: src/video/r3d_scene.h) and Model 3-Vulkan
(camera-first motion, VRR multiples of the arcade's rate).

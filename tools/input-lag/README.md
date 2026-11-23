# input-lag

A Windows test tool: how long a button press takes to reach the screen in a
program's window. It is the same measure for our window and for Supermodel,
so the two can be compared.

## How it measures

- A virtual Xbox 360 pad presses a button. The pad comes from the ViGEmBus
  driver (installed separately), driven through ViGEmClient (MIT, fetched by
  CMake). No keyboard presses are ever sent.
- DXGI desktop duplication watches the window. The first desktop picture
  showing the change is taken, with the time Windows' compositor made it
  (`LastPresentTime`). The press is timed with `QueryPerformanceCounter` just
  before the driver is told, so both times are on the same clock.
- The tool finds the area to watch itself: it holds and lets go of the button
  three times. Pixels that follow the button make the area. Pixels that change
  without it (a frame counter, a notice) are left out. `--shots` writes the
  pictures, with the area in red.
- Presses come at random moments (150-450 ms after the last let-go, by
  default), so they fall at every point of the game's frame and the monitor's
  refresh. Each let-go is timed as well.
- `--self-test` times the tool's own window, which turns white the moment
  XInput sees the button. That is the floor: the pad, XInput and the
  compositor, with no program work in between.

Left out: the USB pad's own polling, and the monitor after the compositor.
The pad's polling is the same for every program. PresentMon's
`msUntilDisplayed` covers part of the monitor stretch (present to display) and
shows each program's present mode. `compare.ps1` records it when given
`-PresentMon`, which needs an elevated PowerShell.

## Building

```
cmake -S tools/input-lag -B build-lag -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-lag
```

You can also add `-DSPIKEOUT_INPUT_LAG=ON` to the main build.

## Running

`compare.ps1` runs the self-test, then each program in turn. Its header lists
what it needs: a save state on the test menu's INPUT TEST screen for our
window, and a Supermodel folder copy set to XInput. For one program:

```
input-lag --cwd DIR --settle 6000 --presses 200 --out ours.csv -- spikeout.exe ROMDIR --load-state input.state
```

The program's window goes on top and takes the focus. If it loses the focus,
the tool pauses until it has it back, because a game reads its pad only while
focused. Leave the PC alone while it runs.

## First results (2026-11-23, i7-8700K, GTX 1080, 100 Hz monitor, windowed 992 x 768)

Three runs each (the second and third by `compare.ps1`):

| | median | p10 | p90 | worst |
|---|---|---|---|---|
| floor (the tool's own window) | 19.2 / 18.1 / 17.9 ms | 15.3 / 14.3 / 13.2 | 22.5 / 22.0 / 21.6 | 24.9 / 23.7 / 23.0 |
| our window (C++ build, zealous-goldberg) | 56.0 / 53.7 / 53.4 ms | 48.9 / 48.4 / 48.4 | 64.6 / 63.4 / 60.7 | 74.1 / 69.9 / 70.0 |
| Supermodel 0.3a-20260528 (VSync 1, OpenGL) | 88.3 / 97.2 / 80.0 ms | 71.3 / 79.0 / 69.0 | 115.6 / 127.9 / 102.3 | 191.8 / 178.7 / 168.6 |

These are presses and let-gos together: 200 samples per program run, about
120 for the floor. XInput saw the virtual pad within 0.6-0.7 ms (median).
Not measured yet: PresentMon (it needs admin rights).

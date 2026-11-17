# Updating from inside the game

The update check (step 1, `src/net/update.*`) asks GitHub's release list at start and says when a newer version
is out. The in-game update (step 2, `src/net/install.*`, the owner's OK of 2026-11-17) installs it: the release
for this computer is downloaded, checked, put in place of the running program and started; the version before
is kept to go back to.

## What the player sees

- At start, when a newer version is out: `VERSION X IS OUT: SEE THE MENU (ESC)`.
- The menu's "Version X is out" (its progress on the right: a percentage, Checking, Ready, Failed) opens the
  update's page:
  - **Install now**: downloads it (the row shows a bar, `MenuItem::progress`, and MB of MB in a box of fixed
    width; Enter stops it), then **Checking the download...**,
    then **Restart as version X**. The notice `VERSION X IS READY: RESTART AS IT IN THE MENU (ESC)` says so if
    the menu was closed meanwhile. Restarting is the player's choice (a game in progress is lost: the page says
    F9 saves the place first).
  - On a failure: **Try again**, and why (`Failed: ...`).
  - **What's new**: the release's page on GitHub (what the item did before).
  - In a room (hosting, joined, watching): "Install it after the network game", no install.
  - When this copy can't install (see below), why, and What's new only.
- The lobby's "N games on another version" opens the same page.
- After the restart: `UPDATED TO VERSION X`. When it failed: `UPDATE TO X FAILED: STILL VERSION W` (and the
  update's page says why: "Tried at the last start: ...").
- Esc > Game > **Go back to version W** while the version before is kept: the game closes and starts again as it
  (`BACK ON VERSION W`); the newer one is deleted (the menu offers it again).

## The download

GitHub's release list already gives each file's `name`, `browser_download_url`, `size` and `digest`
(`sha256:...`, worked out by GitHub at upload). `net::newer_release` takes, from the newest newer release, the
download for this computer, `spikeout-<version>-<platform>.zip` (`.tar.gz` on Linux; platforms `windows-x64`,
`linux-x64`, `macos-arm64`, `macos-x64`, as `tools/package-release.sh` names them), and its signature
`<name>.sig`. A release without both: told, not installable.

`net::install::Download` (libcurl, a thread of its own; `src/net/install_download.cpp`) fetches the signature,
then the download into `spikeout-update/download/` beside the program (the same drive, so that every later step
is a rename), stopping at more bytes than GitHub said.

## The checks

In order; any failure leaves the game as it was (the page says why):

1. The size is GitHub's.
2. The SHA-256 is GitHub's digest (when it gives one). Catches a damaged download.
3. **The project's signature.** The digest comes in the same answer as the link, so it proves nothing if the
   account or the list were tampered with. Each download is signed in the release workflow with the project's
   update key (Monocypher's EdDSA, Ed25519's curve with BLAKE2b) over `"SPKUPD1\n" + file name + "\n" +
   BLAKE2b-512(file)`; the name is signed too, so a signed download can't be offered as another version or
   another system. The secret key is only in the GitHub secret `SPIKEOUT_UPDATE_SECRET`; the public key is built
   in (CMake `SPIKEOUT_UPDATE_KEY`). A build without a key never installs (it only tells).
4. Unpacked (`install::unpack`: zips with our own reader, `src/runtime/zip.cpp`; `.tar.gz` with its inflate and
   a small tar reader), a single top folder left out (the Linux and macOS downloads have one), macOS's
   `__MACOSX`/`._*` extras skipped. Refused: a name leaving the folder (`..`, absolute, `\`, `:`), links and
   devices, more than 512 MB, no program in it.
5. **It starts on this computer**: the program in it run once with `--version` (prints `spikeout <version>` and
   quits before any window, GPU or ROM). It must say the version offered. Catches a Linux build needing a newer
   glibc, a Mac build for another processor or a newer macOS, an antivirus taking the file away.

## Putting it in place

Never written over: renames on one drive, which work on a running program (Windows too: renamed, though not
written or deleted). `install::swap_in`: each of the download's files (the program first) moves the file it
replaces into `spikeout-update/previous/`, then takes its place. The program keeps the name the player gave it
(the download's `spikeout.exe` becomes `my spikeout.exe` if that's what it was called). Files not in the
download (`portable.txt`, anything of the player's) are left alone; a file new in the download is noted as new.
Any failure puts back what was moved. Then `spikeout-update/state.txt` says what was done: `from=`, `to=`,
`file=` (each put in place), `had=` (each that replaced one), `starts=`, `alive=`, `ok=`.

- **Windows**: `spikeout.exe`, README/LICENSE/THIRD-PARTY-NOTICES.
- **Linux** (and the Steam Deck): `spikeout`, the text files, `spikeout.png`, `add-to-menu.sh` (its menu entry
  points at the folder: still right).
- **macOS**: the whole `Spikeout.app` (found from the running program, `Contents/MacOS/spikeout`). Its ad-hoc
  signature travels in the zip; a file downloaded by the game has no quarantine mark, so Gatekeeper doesn't ask
  again. Not installable when macOS runs the app translocated (opened straight from Downloads: a read-only
  hidden copy, `/AppTranslocation/` in its path): the page says to move it to Applications.

## Restarting, and going back on a failure

The window closes as usual (settings saved, the network left, the log closed: the new version's log is
`spikeout.log`), then `install::restart_into`: swap in, start the new program with the same command line plus
`--updated-from W`, and wait up to 60 s for its state to say `alive` (written once its window exists, before the
ROM set is read or the game translated). It stopped, or didn't open a window: it's stopped, everything is put
back (`swap_back`: what it put in moved to `failed/`, the previous files back, the state removed), and this
version is started again with `--update-failed X --update-why WHY`.

A new version that opens its window but stops before its first picture: each start counts (`starts=`) until
the first picture (or a normal close) marks it `ok`. At the third start after two such stops it goes back by
itself (`install::on_start`) and starts the version before with `--update-failed`.

Going back by choice is the same `swap_back`, then the version before started with `--went-back-from X`.
Leftovers (`download/`, `failed/`) are deleted at a start, or once the new version is `ok`; `previous/` stays
until the next update.

## When it isn't offered

- `-dev` builds (they don't check), gamerun, the C++ reference build unless built with a key.
- A build without `SPIKEOUT_UPDATE_KEY`, or without libcurl, or for a system with no downloads.
- A program folder this user can't write in (Program Files, a read-only mount), macOS translocation.
- In a network game.
- `SPIKEOUT_UPDATE_INSTALL=0` (tests).

## Network play

Nothing changes in network play: no sync, speed or protocol change, and installing is only offered outside a
network game. It helps the lobby: a player on an older version can be on the room's version in a minute (the
lobby's "games on another version" line leads to it).

## The key

`tools/update_sign.c` (C only: the release workflow builds it with `cc`):

    update_sign keygen SECRETFILE    a new pair: the secret into SECRETFILE, the public key printed
    update_sign sign FILE...         FILE.sig for each (secret: SPIKEOUT_UPDATE_SECRET, or the file
                                     SPIKEOUT_UPDATE_SECRET_FILE names)
    update_sign check KEY FILE...    each FILE.sig checked

Made once: `keygen`, the public key into CMake's `SPIKEOUT_UPDATE_KEY` default, the secret into the GitHub
secret (`gh secret set SPIKEOUT_UPDATE_SECRET < SECRETFILE`) and a copy kept safe off the repository. Losing it
means a new key, and players update by hand once (their game only trusts the old one). The release workflow
signs the downloads (warns and leaves them unsigned without the secret) and checks the signatures against the
key in `CMakeLists.txt`. `tools/make-release.ps1 -Signer build\update_sign.exe` signs local builds.

## Tests

- `spikeout_tests`, "net install: ...": SHA-256, the asset and its signature found in a release list, signatures
  (a changed file, another name, another key, no key), unpacking (zip with folders and modes, a flat zip,
  .tar.gz, names leaving the folder, links, no program), swapping in, the starts counted, going back (by choice,
  and by itself after two stops).
- On Windows, end to end (2026-11-17) with a local release list (`SPIKEOUT_UPDATE_URL` at a local HTTP server,
  two builds `-DSPIKEOUT_VERSION=0.1.0-test1` and `-test2` with a test key, the menu driven by
  `SPIKEOUT_MENU_KEYS`, which a started program doesn't inherit): installed (downloaded, checked and ready in
  0.6 s; the new version up and `UPDATED TO VERSION 0.1.0-TEST2`, the old files in `previous/`); gone back
  (`BACK ON VERSION 0.1.0-TEST1`, the new file gone); a signed "new version" that stops as it starts: put back
  and `UPDATE TO 0.1.0-TEST3 FAILED: STILL VERSION 0.1.0-TEST1`.
- Linux: built with warnings as errors and the unit tests in WSL. macOS: built by CI; the swap of the app bundle
  to be tried on the owner's Mac.

// The in-game update's installing (docs/updating.md): the release for this system, downloaded by
// net::UpdateDownload (update.hpp), checked, unpacked, put in place of the running program and started; the old
// one kept to go back to.
//
// Where: beside the program (on macOS beside Spikeout.app), a folder spikeout-update holding
//   download/          the download, its signature, and staged/ (it unpacked)
//   previous/          the files the last update replaced (Go back puts them back)
//   failed/            a version taken away (deleted at the next start)
//   state.txt          what the last update did: from=, to=, file= (each replaced), starts=, alive=, ok=
// Every move is a rename on one drive: a running program can be renamed (Windows too) though not overwritten.
// Nothing in the data folder (settings, saves, the translated game) is touched.
#pragma once

#include "net/update.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace net::install {

namespace fs = std::filesystem;

// The download (libcurl, on a thread of its own): the release's file for this system and its signature into
// work/download, checked (its size, GitHub's SHA-256, the project's signature), unpacked into download/staged,
// and the program in it run once (--version: it starts on this computer and is the version offered).
class Download {
public:
    enum class Stage { idle, downloading, checking, ready, failed };
    // Whether this build can install updates at all: libcurl, a key to check them with, downloads for its system.
    static bool available();
    ~Download(); // (stopped, and waited for)
    void start(Release release, fs::path work);
    void cancel(); // (stopped; idle again)
    Stage stage() const { return stage_; }
    uint64_t done() const { return done_; }
    uint64_t total() const { return total_; }
    std::string error() const;
    fs::path staged() const { return work_ / "download" / "staged"; } // (ready: the unpacked release)
    const Release& release() const { return release_; }

private:
    void run();
    void fail(std::string why);
    std::atomic<Stage> stage_{Stage::idle};
    std::atomic<uint64_t> done_{0}, total_{0};
    std::atomic<bool> stop_{false};
    mutable std::mutex lock_;
    std::string error_;
    Release release_;
    fs::path work_;
    std::thread thread_;
};

// The project's public key for the downloads' signatures (CMake SPIKEOUT_UPDATE_KEY, hex); "" none: no installing.
std::string_view public_key();

struct Place {
    fs::path program; // the running program's file
    fs::path unit;    // what an update replaces: the program, or macOS's whole Spikeout.app
    fs::path folder;  // the folder holding the unit (the download's other files go here too)
    fs::path work;    // folder / "spikeout-update"
};
// The running program's (nullopt: it can't be found).
std::optional<Place> here();
// A place made up from a program's path (tests).
Place place_of(const fs::path& program);
// Why this copy can't update itself ("" if it can): macOS running it from a translocated copy, a folder this user
// can't write in.
std::string cant_install(const Place& place);

// The checks: "" if good, else why not.
std::string check_size(const fs::path& file, uint64_t size);
std::string check_digest(const fs::path& file, std::string_view digest); // GitHub's "sha256:<hex>"
std::string check_signature(const fs::path& file, std::string_view name, const fs::path& sig, std::string_view key_hex);

// The archive's files (a .zip, or a .tar.gz) into `into` (emptied first), a single top folder left out (the
// Linux and macOS downloads have one). Names leaving the folder, links and devices refused. Throws
// std::runtime_error with why.
void unpack(const fs::path& archive, const fs::path& into);

// The program file the download unpacked into `staged` (the program in it as this system's downloads name it).
fs::path staged_program(const fs::path& staged);

// Runs `program --version` and waits (20 s at most): "" if it ran, else why not; `out`: what it printed (through
// a file in `scratch`).
std::string run_version(const fs::path& program, const fs::path& scratch, std::string& out);

// What the last update did (state.txt).
struct State {
    std::string from, to;              // the versions
    std::vector<std::string> replaced; // the files put in place (names in the folder)
    std::vector<std::string> had;      // of those, the ones there before (in previous/)
    int starts = 0;                    // the new version's starts so far that haven't reached their first picture
    bool alive = false;                // the new version has opened its window
    bool ok = false;                   // ... and shown its first picture (or closed normally)
};
std::optional<State> read_state(const Place& place);
bool write_state(const Place& place, const State& state);

// The staged files put in place, the ones they replace into previous/ (whatever was there emptied first); a state
// written (from, to, not alive). "" if done; else why, and everything as it was.
std::string swap_in(const Place& place, const fs::path& staged, const std::string& from, const std::string& to);
// The last update undone: what it put in moved to failed/, the previous files back, the state removed. "" if done.
std::string swap_back(const Place& place);
// Whether the last update can be gone back on (its state, and previous/ holding what it replaced).
bool can_go_back(const Place& place);

// Starts `program` with these arguments (UTF-8); waits until the update's state says it's alive, it ends, or
// `seconds` pass (then it's stopped). "" if alive, else why.
std::string start_and_wait(const Place& place, const fs::path& program, const std::vector<std::string>& args, double seconds);
// Starts `program` and doesn't wait. False if it couldn't.
bool start(const fs::path& program, const std::vector<std::string>& args);

// The window's side, in order:
//   at its start: on_start (an update whose new version stopped twice before its first picture: gone back, and the
//   previous version started: the caller quits at once);
//   its window open: mark(alive); its first picture shown, or closed normally: mark(ok).
enum class Start { normal, quit };
Start on_start(const Place& place, const std::vector<std::string>& args);
enum class Mark { alive, ok };
void mark(const Place& place, Mark what);
// Leftovers deleted (failed/, an old download), unless an update is half done.
void tidy(const Place& place);

// Restarting into a downloaded version (the window closed first): swap_in, then the new program started with
// `args` + --updated-from FROM and waited for (60 s). Fails: everything put back and this program's own version
// started again with --update-failed TO --update-why WHY. Returns "" if the new one runs, else why.
std::string restart_into(const Place& place, const fs::path& staged, const std::string& from, const std::string& to,
                         const std::vector<std::string>& args);
// Going back to the previous version: swap_back, then it is started with --went-back-from VERSION.
std::string go_back(const Place& place, const std::vector<std::string>& args);

} // namespace net::install

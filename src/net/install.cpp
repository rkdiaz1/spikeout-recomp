// The in-game update's installing: see install.hpp.
#include "net/install.hpp"

#include "net/digest.hpp"
#include "net/update_sign.h"
#include "runtime/rom.hpp"
#include "runtime/zip.hpp"

#include <monocypher.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace net::install {

namespace {

#ifdef SPIKEOUT_UPDATE_KEY
constexpr const char* kKey = SPIKEOUT_UPDATE_KEY;
#else
constexpr const char* kKey = "";
#endif

// The program as this system's downloads hold it (tools/package-release.sh).
#if defined(_WIN32)
constexpr const char* kProgramInDownload = "spikeout.exe";
#elif defined(__APPLE__)
constexpr const char* kProgramInDownload = "Spikeout.app";
#else
constexpr const char* kProgramInDownload = "spikeout";
#endif

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A rename; false (and `why`) if it failed.
bool move(const fs::path& from, const fs::path& to, std::string* why = nullptr) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec && why) *why = "couldn't move " + utf8(from.filename()) + ": " + ec.message();
    return !ec;
}

void remove_all_quietly(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// Written whole or not at all (a file beside it, then renamed over).
bool write_file(const fs::path& p, const std::string& text) {
    const fs::path tmp = p.string() + ".new";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out.flush()) return false;
    }
    std::error_code ec;
    for (int tries = 0; tries < 50; ++tries) { // (Windows refuses while the other program reads it: a moment later)
        fs::rename(tmp, p, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

std::string_view public_key() { return kKey; }

// ---------------------------------------------------------------------------------------------------------------
// Where the program is

Place place_of(const fs::path& program) {
    Place p;
    p.program = program;
    p.unit = program;
#ifdef __APPLE__
    // .../Spikeout.app/Contents/MacOS/spikeout: the whole bundle is what's replaced
    const fs::path macos = program.parent_path(), contents = macos.parent_path(), bundle = contents.parent_path();
    if (macos.filename() == "MacOS" && contents.filename() == "Contents" && bundle.extension() == ".app") p.unit = bundle;
#endif
    p.folder = p.unit.parent_path();
    p.work = p.folder / "spikeout-update";
    return p;
}

std::optional<Place> here() {
    fs::path program;
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return std::nullopt;
    buf.resize(n);
    program = buf;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::nullopt;
    buf.resize(std::strlen(buf.c_str()));
    std::error_code ec;
    program = fs::canonical(buf, ec);
    if (ec) return std::nullopt;
#else
    std::error_code ec;
    program = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return std::nullopt;
#endif
    return place_of(program);
}

std::string cant_install(const Place& place) {
    if (utf8(place.unit).find("/AppTranslocation/") != std::string::npos)
        return "macOS runs this copy from a hidden folder: move Spikeout to Applications, then start it again";
    std::error_code ec;
    fs::create_directories(place.work, ec);
    const fs::path probe = place.work / "write-test";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out || !(out << "x") || !out.flush()) return "its folder can't be written to (" + utf8(place.folder) + ")";
    }
    fs::remove(probe, ec);
    return {};
}

// ---------------------------------------------------------------------------------------------------------------
// Checks

std::string check_size(const fs::path& file, uint64_t size) {
    std::error_code ec;
    const uint64_t got = fs::file_size(file, ec);
    if (ec) return "the download can't be read";
    if (got != size) return "the download has " + std::to_string(got) + " bytes, not " + std::to_string(size);
    return {};
}

std::string check_digest(const fs::path& file, std::string_view digest) {
    if (digest.substr(0, 7) != "sha256:" || digest.size() != 7 + 64) return "GitHub gave no SHA-256 for it";
    std::ifstream in(file, std::ios::binary);
    if (!in) return "the download can't be read";
    Sha256 sha;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        sha.add(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<std::size_t>(in.gcount()));
    }
    const auto got = sha.finish();
    static const char* kHex = "0123456789abcdef";
    std::string hex;
    for (uint8_t b : got) hex += kHex[b >> 4], hex += kHex[b & 15];
    if (hex != lower(std::string(digest.substr(7)))) return "the download is damaged (its SHA-256 differs from GitHub's)";
    return {};
}

std::string check_signature(const fs::path& file, std::string_view name, const fs::path& sig, std::string_view key_hex) {
    uint8_t key[32];
    if (!spk_update_unhex(key, 32, std::string(key_hex).c_str())) return "this build has no key to check downloads with";
    const std::string signature = read_file(sig);
    if (signature.size() != 64) return "the download's signature is missing";
    std::ifstream in(file, std::ios::binary);
    if (!in) return "the download can't be read";
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 64);
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(buf.data()), static_cast<std::size_t>(in.gcount()));
    }
    uint8_t hash[64];
    crypto_blake2b_final(&ctx, hash);
    uint8_t msg[SPK_UPDATE_MESSAGE_MAX];
    const std::size_t n = spk_update_message(msg, std::string(name).c_str(), hash);
    if (n == 0 || crypto_eddsa_check(reinterpret_cast<const uint8_t*>(signature.data()), key, msg, n) != 0)
        return "the download's signature is wrong (not the project's)";
    return {};
}

// ---------------------------------------------------------------------------------------------------------------
// Unpacking

namespace {

struct Unpacked {
    std::string path; // inside the archive, '/' between folders
    std::vector<uint8_t> data;
    bool runnable = false;
};

constexpr uint64_t kMaxTotal = 512ull << 20;

// A name made safe: its folders, or empty to leave it out (macOS's extras); throws if it would leave the folder.
std::vector<std::string> parts_of(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        const char c = i < path.size() ? path[i] : '/';
        if (c == '\\' || c == ':' || (i == 0 && c == '/')) throw std::runtime_error("a name in the download leaves its folder: " + path);
        if (c != '/') {
            part += c;
            continue;
        }
        if (part == "..") throw std::runtime_error("a name in the download leaves its folder: " + path);
        if (!part.empty() && part != ".") parts.push_back(part);
        part.clear();
    }
    if (!parts.empty() && (parts[0] == "__MACOSX" || parts.back().rfind("._", 0) == 0 || parts.back() == ".DS_Store")) return {};
    return parts;
}

uint64_t octal(const uint8_t* p, std::size_t n) {
    uint64_t v = 0;
    for (std::size_t i = 0; i < n && p[i]; ++i) {
        if (p[i] == ' ') continue;
        if (p[i] < '0' || p[i] > '7') throw std::runtime_error("the download is damaged (a tar header)");
        v = v * 8 + (p[i] - '0');
    }
    return v;
}

std::vector<Unpacked> read_zip(const fs::path& archive) {
    m3::ZipReader zip(archive);
    std::vector<Unpacked> out;
    uint64_t total = 0;
    for (const m3::ZipReader::Entry& e : zip.files()) {
        const uint32_t type = e.mode & 0170000;
        if (type != 0 && type != 0100000) throw std::runtime_error("the download holds a link or a device: " + e.path);
        total += e.size;
        if (total > kMaxTotal) throw std::runtime_error("the download unpacks too big");
        Unpacked u;
        u.path = e.path;
        u.data = zip.read(e);
        u.runnable = (e.mode & 0111) != 0;
        out.push_back(std::move(u));
    }
    return out;
}

std::vector<Unpacked> read_tar_gz(const fs::path& archive) {
    const std::string packed = read_file(archive);
    const auto* g = reinterpret_cast<const uint8_t*>(packed.data());
    const std::size_t n = packed.size();
    if (n < 18 || g[0] != 0x1f || g[1] != 0x8b || g[2] != 8) throw std::runtime_error("the download isn't a .tar.gz");
    const uint8_t flags = g[3];
    std::size_t at = 10;
    auto need = [&](std::size_t k) {
        if (at + k > n - 8) throw std::runtime_error("the download is damaged (gzip)");
    };
    if (flags & 4) { // FEXTRA
        need(2);
        const std::size_t x = g[at] | g[at + 1] << 8;
        at += 2;
        need(x);
        at += x;
    }
    for (int f : {8, 16}) // FNAME, FCOMMENT
        if (flags & f) {
            while (need(1), g[at] != 0) ++at;
            ++at;
        }
    if (flags & 2) need(2), at += 2; // FHCRC
    const uint32_t crc = static_cast<uint32_t>(g[n - 8] | g[n - 7] << 8 | g[n - 6] << 16) | static_cast<uint32_t>(g[n - 5]) << 24;
    const uint32_t size = static_cast<uint32_t>(g[n - 4] | g[n - 3] << 8 | g[n - 2] << 16) | static_cast<uint32_t>(g[n - 1]) << 24;
    if (size > kMaxTotal) throw std::runtime_error("the download unpacks too big");
    std::vector<uint8_t> tar;
    if (!m3::inflate(g + at, n - 8 - at, tar, size) || m3::crc32(tar) != crc)
        throw std::runtime_error("the download is damaged (gzip)");

    std::vector<Unpacked> out;
    std::string long_name;
    std::size_t p = 0;
    while (p + 512 <= tar.size()) {
        const uint8_t* h = &tar[p];
        if (std::all_of(h, h + 512, [](uint8_t b) { return b == 0; })) break;
        const uint64_t size_here = octal(h + 124, 12);
        const char type = static_cast<char>(h[156]);
        p += 512;
        if (size_here > tar.size() - p) throw std::runtime_error("the download is damaged (tar)");
        const uint8_t* data = &tar[p];
        p += static_cast<std::size_t>((size_here + 511) / 512 * 512);
        auto field = [](const uint8_t* f, std::size_t len) { return std::string(reinterpret_cast<const char*>(f), strnlen(reinterpret_cast<const char*>(f), len)); };
        std::string name = field(h, 100);
        if (std::memcmp(h + 257, "ustar", 5) == 0 && h[345]) name = field(h + 345, 155) + "/" + name;
        if (!long_name.empty()) name = long_name, long_name.clear();
        if (type == 'L') { // (GNU: the next file's long name)
            long_name = std::string(reinterpret_cast<const char*>(data), strnlen(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size_here)));
            continue;
        }
        if (type == 'x') { // (pax: its path= replaces the next name)
            std::string rec(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size_here));
            for (std::size_t r = 0; r < rec.size();) {
                const std::size_t sp = rec.find(' ', r);
                const std::size_t len = std::strtoul(rec.c_str() + r, nullptr, 10);
                if (sp == std::string::npos || len == 0 || r + len > rec.size()) break;
                const std::string kv = rec.substr(sp + 1, r + len - sp - 2);
                if (kv.rfind("path=", 0) == 0) long_name = kv.substr(5);
                r += len;
            }
            continue;
        }
        if (type == 'g' || type == '5') continue; // (global pax notes; folders: made as their files need them)
        if (type != '0' && type != '\0' && type != '7') throw std::runtime_error("the download holds a link or a device: " + name);
        Unpacked u;
        u.path = name;
        u.data.assign(data, data + size_here);
        u.runnable = (octal(h + 100, 8) & 0111) != 0;
        out.push_back(std::move(u));
    }
    return out;
}

} // namespace

void unpack(const fs::path& archive, const fs::path& into) {
    const std::string name = lower(utf8(archive.filename()));
    std::vector<Unpacked> files;
    if (name.size() > 4 && name.ends_with(".zip")) files = read_zip(archive);
    else if (name.ends_with(".tar.gz") || name.ends_with(".tgz")) files = read_tar_gz(archive);
    else throw std::runtime_error("a download this game can't unpack: " + utf8(archive.filename()));
    // each file's folders, macOS's extras left out
    std::vector<std::pair<std::vector<std::string>, Unpacked*>> named;
    for (Unpacked& u : files) {
        auto parts = parts_of(u.path);
        if (!parts.empty()) named.emplace_back(std::move(parts), &u);
    }
    if (named.empty()) throw std::runtime_error("the download is empty");
    // a single top folder holding everything: left out
    bool one_top = true;
    for (const auto& [parts, u] : named) one_top = one_top && parts.size() >= 2 && parts[0] == named[0].first[0];
    if (one_top)
        for (auto& [parts, u] : named) parts.erase(parts.begin());
    remove_all_quietly(into);
    fs::create_directories(into);
    for (const auto& [parts, u] : named) {
        fs::path path = into;
        for (const std::string& part : parts) path /= from_utf8(part);
        fs::create_directories(path.parent_path());
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(u->data.data()), static_cast<std::streamsize>(u->data.size()));
            if (!out.flush()) throw std::runtime_error("couldn't write " + utf8(path.filename()));
        }
#ifndef _WIN32
        ::chmod(path.c_str(), u->runnable ? 0755 : 0644);
#endif
    }
    if (!fs::exists(staged_program(into))) throw std::runtime_error(std::string("the download holds no ") + kProgramInDownload);
}

fs::path staged_program(const fs::path& staged) {
#ifdef __APPLE__
    return staged / kProgramInDownload / "Contents" / "MacOS" / "spikeout";
#else
    return staged / kProgramInDownload;
#endif
}

// ---------------------------------------------------------------------------------------------------------------
// Processes

namespace {

#ifdef _WIN32
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One argument as CommandLineToArgvW reads it back.
void quote_onto(std::wstring& line, const std::wstring& arg) {
    if (!line.empty()) line += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += arg;
        return;
    }
    line += L'"';
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') ++i, ++slashes;
        if (i == arg.size()) {
            line.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') line.append(slashes * 2 + 1, L'\\'), line += L'"';
        else line.append(slashes, L'\\'), line += arg[i];
    }
    line += L'"';
}

struct Child {
    HANDLE process = nullptr;
    bool ended(DWORD wait_ms = 0) const { return WaitForSingleObject(process, wait_ms) == WAIT_OBJECT_0; }
    void stop() const { TerminateProcess(process, 1); }
    ~Child() {
        if (process) CloseHandle(process);
    }
};

bool spawn(Child& child, const fs::path& program, const std::vector<std::string>& args, HANDLE out = nullptr) {
    std::wstring line;
    quote_onto(line, program.wstring());
    for (const std::string& a : args) quote_onto(line, wide(a));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (out) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = out;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(program.wstring().c_str(), line.data(), nullptr, nullptr, out ? TRUE : FALSE, out ? CREATE_NO_WINDOW : 0,
                        nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    child.process = pi.hProcess;
    return true;
}
#else
struct Child {
    pid_t pid = -1;
    mutable bool done = false;
    bool ended(int wait_ms = 0) const {
        if (done) return true;
        for (int waited = 0;; waited += 10) {
            int status = 0;
            if (waitpid(pid, &status, WNOHANG) != 0) return done = true;
            if (waited >= wait_ms) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    void stop() const {
        if (!done) kill(pid, SIGKILL), waitpid(pid, nullptr, 0), done = true;
    }
};

bool spawn(Child& child, const fs::path& program, const std::vector<std::string>& args, const char* out = nullptr) {
    std::vector<std::string> all{program.string()};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (out) {
        posix_spawn_file_actions_addopen(&actions, 1, out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
    }
    const int r = posix_spawn(&child.pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    return r == 0;
}
#endif

} // namespace

std::string run_version(const fs::path& program, const fs::path& scratch, std::string& out) {
    out.clear();
    const fs::path file = scratch / "version-check.txt";
    Child child;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE h = CreateFileW(file.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "couldn't make a file to run it with";
    const bool started = spawn(child, program, {"--version"}, h);
    CloseHandle(h);
#else
    const bool started = spawn(child, program, {"--version"}, file.c_str());
#endif
    if (!started) return "it couldn't be started on this computer";
    if (!child.ended(20000)) {
        child.stop();
        return "it didn't answer when started";
    }
#ifdef _WIN32
    DWORD code = 0;
    GetExitCodeProcess(child.process, &code);
#endif
    out = read_file(file);
    std::error_code ec;
    fs::remove(file, ec);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
#ifdef _WIN32
    if (code != 0) return "it stopped with an error when started (" + std::to_string(code) + ")";
#endif
    if (out.empty()) return "it didn't start on this computer";
    return {};
}

bool start(const fs::path& program, const std::vector<std::string>& args) {
    Child child;
    if (!spawn(child, program, args)) return false;
#ifndef _WIN32
    child.done = true; // (not waited for: it outlives this program)
#endif
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The state, swapping

std::optional<State> read_state(const Place& place) {
    std::ifstream in(place.work / "state.txt", std::ios::binary);
    if (!in) return std::nullopt;
    State s;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "from") s.from = v;
        else if (k == "to") s.to = v;
        else if (k == "file") s.replaced.push_back(v);
        else if (k == "had") s.had.push_back(v);
        else if (k == "starts") s.starts = std::atoi(v.c_str());
        else if (k == "alive") s.alive = v == "1";
        else if (k == "ok") s.ok = v == "1";
    }
    if (s.to.empty() || s.replaced.empty()) return std::nullopt;
    return s;
}

bool write_state(const Place& place, const State& s) {
    std::string text = "# The last in-game update (docs/updating.md)\nfrom=" + s.from + "\nto=" + s.to + "\n";
    for (const std::string& f : s.replaced) text += "file=" + f + "\n";
    for (const std::string& f : s.had) text += "had=" + f + "\n";
    text += "starts=" + std::to_string(s.starts) + "\nalive=" + (s.alive ? "1" : "0") + "\nok=" + (s.ok ? "1" : "0") + "\n";
    std::error_code ec;
    fs::create_directories(place.work, ec);
    return write_file(place.work / "state.txt", text);
}

std::string swap_in(const Place& place, const fs::path& staged, const std::string& from, const std::string& to) {
    const fs::path previous = place.work / "previous";
    remove_all_quietly(previous);
    std::error_code ec;
    fs::create_directories(previous, ec);
    if (ec) return "couldn't make " + utf8(previous);
    State s;
    s.from = from, s.to = to;
    // each of the download's files, the program first (its name here may differ: the download's is renamed to it)
    std::vector<std::pair<fs::path, std::string>> items; // (staged file, its name here)
    for (const fs::directory_entry& e : fs::directory_iterator(staged)) {
        const std::string name = utf8(e.path().filename());
        if (name == kProgramInDownload) items.insert(items.begin(), {e.path(), utf8(place.unit.filename())});
        else items.emplace_back(e.path(), name);
    }
    if (items.empty() || utf8(items[0].first.filename()) != kProgramInDownload) return "the download holds no program";
    std::string why;
    std::vector<std::pair<std::string, bool>> done; // (name here, whether one was there before)
    auto undo = [&] {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            const fs::path here = place.folder / from_utf8(it->first);
            fs::path back_to = staged / from_utf8(it->first);
            if (it->first == utf8(place.unit.filename())) back_to = staged / kProgramInDownload;
            move(here, back_to);
            if (it->second) move(previous / from_utf8(it->first), here);
        }
    };
    for (const auto& [from_path, name] : items) {
        const fs::path here = place.folder / from_utf8(name);
        const bool had = fs::exists(here, ec) || fs::is_symlink(here, ec);
        if (had && !move(here, previous / from_utf8(name), &why)) {
            undo();
            return why;
        }
        if (!move(from_path, here, &why)) {
            if (had) move(previous / from_utf8(name), here);
            undo();
            return why;
        }
        done.emplace_back(name, had);
        s.replaced.push_back(name);
        if (had) s.had.push_back(name);
    }
    if (!write_state(place, s)) {
        undo();
        return "couldn't write " + utf8(place.work / "state.txt");
    }
    return {};
}

bool can_go_back(const Place& place) {
    const auto s = read_state(place);
    if (!s || s->had.empty()) return false;
    std::error_code ec;
    for (const std::string& f : s->had)
        if (!fs::exists(place.work / "previous" / from_utf8(f), ec)) return false;
    return std::find(s->had.begin(), s->had.end(), utf8(place.unit.filename())) != s->had.end();
}

std::string swap_back(const Place& place) {
    const auto s = read_state(place);
    if (!s) return "there's no update to go back on";
    const fs::path previous = place.work / "previous", failed = place.work / "failed";
    remove_all_quietly(failed);
    std::error_code ec;
    fs::create_directories(failed, ec);
    std::string why;
    for (const std::string& f : s->replaced) {
        const fs::path here = place.folder / from_utf8(f);
        if (fs::exists(here, ec) && !move(here, failed / from_utf8(f), &why)) return why;
        const bool had = std::find(s->had.begin(), s->had.end(), f) != s->had.end();
        if (had && !move(previous / from_utf8(f), here, &why)) return why;
    }
    fs::remove(place.work / "state.txt", ec);
    remove_all_quietly(failed); // (a program still running can't be deleted on Windows: then at the next start)
    return {};
}

std::string start_and_wait(const Place& place, const fs::path& program, const std::vector<std::string>& args, double seconds) {
    Child child;
    if (!spawn(child, program, args)) return "it couldn't be started";
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        if (const auto s = read_state(place); s && s->alive) {
#ifndef _WIN32
            child.done = true; // (left running: it outlives this program)
#endif
            return {};
        }
        if (child.ended(100)) return "it stopped as it started";
    }
    child.stop();
    return "it didn't open its window";
}

// ---------------------------------------------------------------------------------------------------------------
// The window's side

Start on_start(const Place& place, const std::vector<std::string>& args) {
    auto s = read_state(place);
    if (!s || s->ok) return Start::normal;
    if (s->starts >= 2) {
        // The new version stopped twice before its first picture: back to the one before, which is started.
        std::printf("spikeout: version %s stopped twice before its first picture: going back to %s\n", s->to.c_str(), s->from.c_str());
        const std::string to = s->to;
        if (const std::string why = swap_back(place); !why.empty()) {
            std::printf("spikeout: couldn't go back: %s\n", why.c_str());
            s->ok = true; // (not tried again at every start)
            write_state(place, *s);
            return Start::normal;
        }
        std::vector<std::string> a = args;
        a.insert(a.end(), {"--update-failed", to, "--update-why", "it stopped twice before its first picture"});
        if (start(place.program, a)) return Start::quit;
        return Start::normal;
    }
    ++s->starts;
    write_state(place, *s);
    return Start::normal;
}

void mark(const Place& place, Mark what) {
    auto s = read_state(place);
    if (!s || s->ok) return;
    if (what == Mark::alive) {
        if (s->alive) return;
        s->alive = true;
    } else {
        s->alive = s->ok = true, s->starts = 0;
    }
    write_state(place, *s);
    if (what == Mark::ok) tidy(place); // (the download, and a version gone back from, not needed now)
}

void tidy(const Place& place) {
    std::error_code ec;
    if (!fs::exists(place.work, ec)) return;
    if (const auto s = read_state(place); s && !s->ok) return; // (half done: left for on_start)
    remove_all_quietly(place.work / "failed");
    remove_all_quietly(place.work / "download");
}

std::string restart_into(const Place& place, const fs::path& staged, const std::string& from, const std::string& to,
                         const std::vector<std::string>& args) {
    std::string why = swap_in(place, staged, from, to);
    if (why.empty()) {
        std::vector<std::string> a = args;
        a.insert(a.end(), {"--updated-from", from});
        why = start_and_wait(place, place.program, a, 60);
        if (why.empty()) return {};
        std::printf("spikeout: version %s %s: going back\n", to.c_str(), why.c_str());
        if (const std::string back = swap_back(place); !back.empty()) {
            std::printf("spikeout: couldn't go back: %s\n", back.c_str());
            return why + "; and going back failed: " + back;
        }
    }
    std::printf("spikeout: the update to %s failed: %s\n", to.c_str(), why.c_str());
    std::vector<std::string> a = args;
    a.insert(a.end(), {"--update-failed", to, "--update-why", why});
    start(place.program, a);
    return why;
}

std::string go_back(const Place& place, const std::vector<std::string>& args) {
    const auto s = read_state(place);
    if (!s) return "there's no update to go back on";
    if (const std::string why = swap_back(place); !why.empty()) return why;
    std::vector<std::string> a = args;
    a.insert(a.end(), {"--went-back-from", s->to});
    if (!start(place.program, a)) return "the previous version couldn't be started";
    return {};
}

} // namespace net::install

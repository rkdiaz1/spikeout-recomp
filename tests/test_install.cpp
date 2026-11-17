// The in-game update (src/net/install.hpp, docs/updating.md): its checks, unpacking, swapping and going back, in a
// folder of their own under the system's temporary folder.
#include "test.hpp"

#include "net/digest.hpp"
#include "net/install.hpp"
#include "net/update.hpp"
#include "net/update_sign.h"
#include "runtime/rom.hpp"

#include <monocypher.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace net;
namespace fs = std::filesystem;

namespace {

std::string hex(const uint8_t* p, std::size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) s += k[p[i] >> 4], s += k[p[i] & 15];
    return s;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

// A folder of the test's own, empty.
fs::path fresh(const std::string& name) {
    const fs::path p = fs::temp_directory_path() / ("spikeout-test-install-" + name);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void le16(std::string& s, uint32_t v) { s += static_cast<char>(v & 0xFF), s += static_cast<char>(v >> 8 & 0xFF); }
void le32(std::string& s, uint32_t v) { le16(s, v & 0xFFFF), le16(s, v >> 16); }

struct File {
    std::string name, data;
    uint32_t mode = 0100644;
};

// A zip of stored files, made on a Unix (their modes kept), as macOS's ditto makes them.
std::string make_zip(const std::vector<File>& files) {
    std::string out, dir;
    for (const File& f : files) {
        const uint32_t crc = m3::crc32({reinterpret_cast<const uint8_t*>(f.data.data()), f.data.size()});
        const auto at = static_cast<uint32_t>(out.size());
        le32(out, 0x04034B50), le16(out, 20), le16(out, 0), le16(out, 0), le16(out, 0), le16(out, 0);
        le32(out, crc), le32(out, static_cast<uint32_t>(f.data.size())), le32(out, static_cast<uint32_t>(f.data.size()));
        le16(out, static_cast<uint32_t>(f.name.size())), le16(out, 0);
        out += f.name + f.data;
        le32(dir, 0x02014B50), le16(dir, 3 << 8 | 20), le16(dir, 20), le16(dir, 0), le16(dir, 0), le16(dir, 0), le16(dir, 0);
        le32(dir, crc), le32(dir, static_cast<uint32_t>(f.data.size())), le32(dir, static_cast<uint32_t>(f.data.size()));
        le16(dir, static_cast<uint32_t>(f.name.size())), le16(dir, 0), le16(dir, 0), le16(dir, 0), le16(dir, 0);
        le32(dir, f.mode << 16), le32(dir, at);
        dir += f.name;
    }
    const auto dir_at = static_cast<uint32_t>(out.size());
    out += dir;
    le32(out, 0x06054B50), le16(out, 0), le16(out, 0), le16(out, static_cast<uint32_t>(files.size())),
        le16(out, static_cast<uint32_t>(files.size())), le32(out, static_cast<uint32_t>(dir.size())), le32(out, dir_at), le16(out, 0);
    return out;
}

// A .tar.gz (ustar, deflate's stored blocks), as the Linux download is.
std::string make_tar_gz(const std::vector<File>& files) {
    std::string tar;
    for (const File& f : files) {
        std::string h(512, '\0');
        h.replace(0, f.name.size(), f.name);
        char num[16];
        std::snprintf(num, sizeof num, "%07o", static_cast<unsigned>(f.mode & 07777));
        h.replace(100, 7, num);
        std::snprintf(num, sizeof num, "%011o", static_cast<unsigned>(f.data.size()));
        h.replace(124, 11, num);
        h[156] = '0';
        h.replace(257, 6, std::string("ustar\0", 6));
        tar += h + f.data + std::string((512 - f.data.size() % 512) % 512, '\0');
    }
    tar += std::string(1024, '\0');
    std::string gz = "\x1f\x8b\x08";
    gz += std::string(7, '\0');
    for (std::size_t at = 0; at < tar.size(); at += 65535) { // (stored blocks)
        const auto n = static_cast<uint32_t>(std::min<std::size_t>(65535, tar.size() - at));
        gz += static_cast<char>(at + n == tar.size() ? 1 : 0);
        le16(gz, n), le16(gz, ~n & 0xFFFF);
        gz += tar.substr(at, n);
    }
    le32(gz, m3::crc32({reinterpret_cast<const uint8_t*>(tar.data()), tar.size()}));
    le32(gz, static_cast<uint32_t>(tar.size()));
    return gz;
}

#ifdef _WIN32
constexpr const char* kProgram = "spikeout.exe";
#elif defined(__APPLE__)
constexpr const char* kProgram = "Spikeout.app/Contents/MacOS/spikeout";
#else
constexpr const char* kProgram = "spikeout";
#endif

} // namespace

TEST_CASE("net install: SHA-256 as FIPS 180-4's examples, in pieces too") {
    const std::string abc = "abc";
    CHECK_EQ(hex(sha256(bytes_of(abc)).data(), 32), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(hex(sha256(bytes_of("")).data(), 32), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha256 s;
    for (char c : two) s.add(reinterpret_cast<const uint8_t*>(&c), 1);
    const auto d = s.finish();
    CHECK_EQ(hex(d.data(), 32), std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST_CASE("net install: a release's download for this system found with its signature, else none") {
    const std::string name = download_name("0.2.0");
    if (name.empty()) return; // (a system with no downloads)
    const std::string list = R"([{"tag_name":"v0.2.0","draft":false,"prerelease":false,"html_url":"page","assets":[
        {"name":")" + name + R"(","browser_download_url":"u1","size":1234,"digest":"sha256:ab","state":"uploaded"},
        {"name":")" + name + R"(.sig","browser_download_url":"u2","size":64},
        {"name":"spikeout-0.2.0-other-x64.zip","browser_download_url":"u3","size":9}]}])";
    const auto r = newer_release(*Json::parse(list), "0.1.0");
    CHECK(r.has_value());
    CHECK_EQ(r->download, name);
    CHECK_EQ(r->download_url, std::string("u1"));
    CHECK_EQ(r->signature_url, std::string("u2"));
    CHECK_EQ(r->download_size, uint64_t{1234});
    CHECK_EQ(r->digest, std::string("sha256:ab"));
    // no signature beside it: not installable (still told)
    const std::string unsigned_list = R"([{"tag_name":"v0.2.0","draft":false,"prerelease":false,"html_url":"page","assets":[
        {"name":")" + name + R"(","browser_download_url":"u1","size":1234}]}])";
    const auto u = newer_release(*Json::parse(unsigned_list), "0.1.0");
    CHECK(u.has_value());
    CHECK(u->download.empty());
}

TEST_CASE("net install: a download's signature checked, with its name") {
    const fs::path dir = fresh("sign");
    const std::string name = "spikeout-0.2.0-windows-x64.zip";
    write(dir / name, "the download's bytes");
    uint8_t seed[32], secret[64], pub[32];
    for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(i * 7 + 1);
    crypto_eddsa_key_pair(secret, pub, seed);
    auto sign = [&](const fs::path& file, const std::string& as) {
        const std::string data = read(file);
        uint8_t hash[64], msg[SPK_UPDATE_MESSAGE_MAX], sig[64];
        crypto_blake2b(hash, 64, reinterpret_cast<const uint8_t*>(data.data()), data.size());
        const std::size_t n = spk_update_message(msg, as.c_str(), hash);
        crypto_eddsa_sign(sig, secret, msg, n);
        write(file.string() + ".sig", std::string(reinterpret_cast<const char*>(sig), 64));
    };
    const std::string key = hex(pub, 32);
    sign(dir / name, name);
    CHECK_EQ(install::check_signature(dir / name, name, dir / (name + ".sig"), key), std::string());
    // offered under another name (another version's): refused
    CHECK(!install::check_signature(dir / name, "spikeout-0.3.0-windows-x64.zip", dir / (name + ".sig"), key).empty());
    // changed: refused
    write(dir / name, "the download's bytes, changed");
    CHECK(!install::check_signature(dir / name, name, dir / (name + ".sig"), key).empty());
    // another key: refused
    sign(dir / name, name);
    CHECK(!install::check_signature(dir / name, name, dir / (name + ".sig"), std::string(64, '1')).empty());
    // no key in the build: refused
    CHECK(!install::check_signature(dir / name, name, dir / (name + ".sig"), "").empty());
    // the digest
    const std::string data = read(dir / name);
    const auto d = sha256(bytes_of(data));
    CHECK_EQ(install::check_digest(dir / name, "sha256:" + hex(d.data(), 32)), std::string());
    CHECK(!install::check_digest(dir / name, "sha256:" + std::string(64, '0')).empty());
    CHECK_EQ(install::check_size(dir / name, data.size()), std::string());
    CHECK(!install::check_size(dir / name, data.size() + 1).empty());
    fs::remove_all(dir);
}

TEST_CASE("net install: downloads unpacked, their one top folder left out, names leaving the folder refused") {
    const fs::path dir = fresh("unpack");
    // a zip with a top folder, macOS's extras, a program
    write(dir / "a.zip", make_zip({{std::string("top/") + kProgram, "program", 0100755},
                                   {"top/README.txt", "readme"},
                                   {"top/sub/x.txt", "x"},
                                   {"__MACOSX/top/._README.txt", "junk"}}));
    install::unpack(dir / "a.zip", dir / "staged");
    CHECK_EQ(read(install::staged_program(dir / "staged")), std::string("program"));
    CHECK_EQ(read(dir / "staged" / "README.txt"), std::string("readme"));
    CHECK_EQ(read(dir / "staged" / "sub" / "x.txt"), std::string("x"));
    CHECK(!fs::exists(dir / "staged" / "__MACOSX"));
#ifndef _WIN32
    CHECK((fs::status(install::staged_program(dir / "staged")).permissions() & fs::perms::owner_exec) != fs::perms::none);
#endif
    // a flat zip (Windows'): as it is
    write(dir / "b.zip", make_zip({{kProgram, "flat"}, {"LICENSE.txt", "gpl"}}));
    install::unpack(dir / "b.zip", dir / "staged");
    CHECK_EQ(read(install::staged_program(dir / "staged")), std::string("flat"));
    CHECK(!fs::exists(dir / "staged" / "README.txt")); // (emptied first)
    // a .tar.gz (Linux's)
    write(dir / "c.tar.gz", make_tar_gz({{std::string("spikeout-1-linux-x64/") + kProgram, "tarred", 0100755},
                                         {"spikeout-1-linux-x64/spikeout.png", std::string(70000, 'p')}}));
    install::unpack(dir / "c.tar.gz", dir / "staged");
    CHECK_EQ(read(install::staged_program(dir / "staged")), std::string("tarred"));
    CHECK_EQ(read(dir / "staged" / "spikeout.png").size(), std::size_t{70000});
    // refused: a name leaving the folder, a link, no program
    auto refused = [&](const fs::path& archive) {
        try {
            install::unpack(archive, dir / "bad");
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    write(dir / "d.zip", make_zip({{kProgram, "p"}, {"../escape.txt", "x"}}));
    CHECK(refused(dir / "d.zip"));
    CHECK(!fs::exists(dir / "escape.txt"));
    write(dir / "e.zip", make_zip({{kProgram, "p"}, {"link", "/etc/passwd", 0120777}}));
    CHECK(refused(dir / "e.zip"));
    write(dir / "f.zip", make_zip({{"README.txt", "no program"}}));
    CHECK(refused(dir / "f.zip"));
    write(dir / "g.tar.gz", make_tar_gz({{"/abs.txt", "x"}}));
    CHECK(refused(dir / "g.tar.gz"));
    fs::remove_all(dir);
}

#ifndef __APPLE__ // (macOS swaps a whole Spikeout.app: the same code, a folder instead of a file)
TEST_CASE("net install: a download swapped in, the old files kept, and gone back on") {
    const fs::path dir = fresh("swap");
    const fs::path folder = dir / "game";
    // the game as installed: its program (under a name of the player's own), a readme, a file of the player's
    const std::string own_name = std::string("my spikeout") + (fs::path(kProgram).extension().string());
    write(folder / own_name, "old program");
    write(folder / "README.txt", "old readme");
    write(folder / "portable.txt", "");
    const install::Place place = install::place_of(folder / own_name);
    CHECK(place.work == folder / "spikeout-update");
    // the download, unpacked: the program, the readme, a file new in it
    const fs::path staged = place.work / "download" / "staged";
    write(staged / kProgram, "new program");
    write(staged / "README.txt", "new readme");
    write(staged / "NEW.txt", "new file");
    CHECK_EQ(install::swap_in(place, staged, "0.1.0", "0.2.0"), std::string());
    CHECK_EQ(read(folder / own_name), std::string("new program")); // (renamed as the player had it)
    CHECK_EQ(read(folder / "README.txt"), std::string("new readme"));
    CHECK_EQ(read(folder / "NEW.txt"), std::string("new file"));
    CHECK(fs::exists(folder / "portable.txt")); // (not the download's: left alone)
    CHECK_EQ(read(place.work / "previous" / own_name), std::string("old program"));
    auto s = install::read_state(place);
    CHECK(s.has_value());
    CHECK_EQ(s->from, std::string("0.1.0"));
    CHECK_EQ(s->to, std::string("0.2.0"));
    CHECK(!s->alive && !s->ok);
    CHECK(install::can_go_back(place));
    // the new version's starts: counted until its first picture
    CHECK(install::on_start(place, {}) == install::Start::normal);
    CHECK_EQ(install::read_state(place)->starts, 1);
    install::mark(place, install::Mark::alive);
    CHECK(install::read_state(place)->alive);
    install::mark(place, install::Mark::ok);
    CHECK(install::read_state(place)->ok);
    CHECK(install::on_start(place, {}) == install::Start::normal); // (went well: nothing more)
    CHECK_EQ(install::read_state(place)->starts, 0);
    // going back: the old files again, the new one gone
    CHECK_EQ(install::swap_back(place), std::string());
    CHECK_EQ(read(folder / own_name), std::string("old program"));
    CHECK_EQ(read(folder / "README.txt"), std::string("old readme"));
    CHECK(!fs::exists(folder / "NEW.txt"));
    CHECK(fs::exists(folder / "portable.txt"));
    CHECK(!install::read_state(place).has_value());
    CHECK(!install::can_go_back(place));
    // a new version that stopped twice before its first picture: gone back on at the third start
    fs::remove_all(staged);
    write(staged / kProgram, "new program");
    write(staged / "README.txt", "new readme");
    CHECK_EQ(install::swap_in(place, staged, "0.1.0", "0.2.0"), std::string());
    CHECK(install::on_start(place, {}) == install::Start::normal);
    CHECK(install::on_start(place, {}) == install::Start::normal);
    CHECK_EQ(install::read_state(place)->starts, 2);
    CHECK_EQ(read(folder / own_name), std::string("new program"));
    install::on_start(place, {}); // (gone back; the old program started: here a text file, which can't be)
    CHECK_EQ(read(folder / own_name), std::string("old program"));
    CHECK_EQ(read(folder / "README.txt"), std::string("old readme"));
    CHECK(!install::read_state(place).has_value());
    // a download holding no program: refused, nothing changed
    fs::remove_all(staged);
    write(staged / "README.txt", "only a readme");
    CHECK(!install::swap_in(place, staged, "0.1.0", "0.2.0").empty());
    CHECK_EQ(read(folder / "README.txt"), std::string("old readme"));
    fs::remove_all(dir);
}
#endif

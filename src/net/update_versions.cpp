// The update check's version rules (src/net/update.hpp), built with or
// without libcurl (the unit tests use them).
#include "net/update.hpp"

#include <cctype>
#include <cstdlib>

namespace net {

namespace {

struct Parsed {
    unsigned long numbers[3] = {0, 0, 0};
    std::string label;       // its letters ("preview")
    unsigned long count = 0; // its number (1 of "preview1")
    bool labelled = false;
};

Parsed parse(std::string_view v) {
    Parsed p;
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.remove_prefix(1);
    std::size_t at = 0;
    for (int i = 0; i < 3 && at < v.size(); ++i) {
        unsigned long n = 0;
        while (at < v.size() && std::isdigit(static_cast<unsigned char>(v[at]))) n = n * 10 + static_cast<unsigned long>(v[at++] - '0');
        p.numbers[i] = n;
        if (at < v.size() && v[at] == '.') ++at;
        else break;
    }
    if (at < v.size() && (v[at] == '-' || v[at] == '+')) {
        p.labelled = v[at] == '-';
        ++at;
        while (at < v.size() && !std::isdigit(static_cast<unsigned char>(v[at])))
            p.label += static_cast<char>(std::tolower(static_cast<unsigned char>(v[at++])));
        while (at < v.size() && std::isdigit(static_cast<unsigned char>(v[at]))) p.count = p.count * 10 + static_cast<unsigned long>(v[at++] - '0');
    }
    return p;
}

} // namespace

int compare_versions(std::string_view a, std::string_view b) {
    const Parsed x = parse(a), y = parse(b);
    for (int i = 0; i < 3; ++i)
        if (x.numbers[i] != y.numbers[i]) return x.numbers[i] < y.numbers[i] ? -1 : 1;
    if (x.labelled != y.labelled) return x.labelled ? -1 : 1; // (a test build before its release)
    if (const int c = x.label.compare(y.label); c != 0) return c < 0 ? -1 : 1;
    if (x.count != y.count) return x.count < y.count ? -1 : 1;
    return 0;
}

bool prerelease(std::string_view version) { return parse(version).labelled; }

bool dev_build(std::string_view version) { return parse(version).label == "dev"; }

std::optional<Release> newer_release(const Json& releases, std::string_view current) {
    std::optional<Release> best;
    for (const Json& r : releases.items()) {
        if (r["draft"].as_bool()) continue;
        std::string tag = r["tag_name"].as_string();
        if (tag.empty()) continue;
        if (tag[0] == 'v' || tag[0] == 'V') tag.erase(0, 1);
        if (r["prerelease"].as_bool() && !prerelease(current)) continue; // (test builds only offered to test builds)
        if (compare_versions(tag, current) <= 0) continue;
        if (best && compare_versions(tag, best->version) <= 0) continue;
        best = Release{};
        best->version = tag, best->page = r["html_url"].as_string();
        // its download for this system, and that download's signature
        const std::string name = download_name(tag);
        const Json* file = nullptr;
        const Json* sig = nullptr;
        if (!name.empty() && r["assets"].is_array())
            for (const Json& a : r["assets"].items()) {
                if (a["name"].as_string() == name) file = &a;
                if (a["name"].as_string() == name + ".sig") sig = &a;
            }
        if (file && sig && ((*file)["state"].as_string().empty() || (*file)["state"].as_string() == "uploaded")) {
            best->download = name;
            best->download_url = (*file)["browser_download_url"].as_string();
            best->download_size = static_cast<uint64_t>((*file)["size"].as_number(0));
            best->digest = (*file)["digest"].as_string();
            best->signature_url = (*sig)["browser_download_url"].as_string();
            if (best->download_url.empty() || best->signature_url.empty() || best->download_size == 0) best->download.clear();
        }
    }
    return best;
}

const char* platform() {
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
    return "windows-x64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "macos-x64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#else
    return "";
#endif
}

std::string download_name(std::string_view version) {
    const std::string p = platform();
    if (p.empty() || version.empty()) return {};
    return "spikeout-" + std::string(version) + "-" + p + (p.rfind("linux", 0) == 0 ? ".tar.gz" : ".zip");
}

} // namespace net

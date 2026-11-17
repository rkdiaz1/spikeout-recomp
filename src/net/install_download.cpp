// The in-game update's download (install.hpp): GitHub's release files over HTTPS through libcurl.
#include "net/install.hpp"
#include "net/ca_file.hpp"

#include <cstdio>
#include <curl/curl.h>
#include <fstream>

namespace net::install {

namespace {

struct Sink {
    std::ofstream* file = nullptr; // (the download) or
    std::string* text = nullptr;   // (the signature)
    uint64_t limit = 0, got = 0;
};

size_t keep(char* data, size_t size, size_t count, void* out) {
    auto* s = static_cast<Sink*>(out);
    const size_t n = size * count;
    s->got += n;
    if (s->got > s->limit) return 0; // (more than GitHub said: stopped)
    if (s->file) s->file->write(data, static_cast<std::streamsize>(n));
    else s->text->append(data, n);
    return n;
}

struct Progress {
    std::atomic<uint64_t>* done;
    std::atomic<bool>* stop;
};

int progress(void* p, curl_off_t, curl_off_t now, curl_off_t, curl_off_t) {
    auto* pr = static_cast<Progress*>(p);
    *pr->done = static_cast<uint64_t>(now);
    return *pr->stop ? 1 : 0;
}

// One file from `url` into the sink; "" if done, else why.
std::string fetch(const std::string& url, Sink& sink, Progress* pr) {
    CURL* c = curl_easy_init();
    if (!c) return "no connection";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, "spikeout-recomp");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); // (GitHub's links lead to its file store)
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L); // (under 1 KB/s for 30 s: given up)
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT | CURLSSLOPT_NATIVE_CA));
    set_ca_file(c);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, keep);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    if (pr) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, pr);
    }
    std::string why;
    long status = 0;
    const CURLcode r = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (r == CURLE_ABORTED_BY_CALLBACK) why = "stopped";
    else if (r == CURLE_WRITE_ERROR && sink.got > sink.limit) why = "the download is bigger than GitHub said";
    else if (r != CURLE_OK) why = curl_easy_strerror(r);
    else if (status != 200) why = "GitHub answered " + std::to_string(status);
    curl_easy_cleanup(c);
    return why;
}

} // namespace

bool Download::available() { return !public_key().empty() && *platform() != '\0'; }

Download::~Download() { cancel(); }

void Download::cancel() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    stop_ = false;
    if (stage_ != Stage::ready) stage_ = Stage::idle;
}

std::string Download::error() const {
    std::lock_guard guard(lock_);
    return error_;
}

void Download::fail(std::string why) {
    std::printf("spikeout: the update to %s: %s\n", release_.version.c_str(), why.c_str());
    {
        std::lock_guard guard(lock_);
        error_ = std::move(why);
    }
    stage_ = Stage::failed;
}

void Download::start(Release release, fs::path work) {
    cancel();
    release_ = std::move(release);
    work_ = std::move(work);
    {
        std::lock_guard guard(lock_);
        error_.clear();
    }
    done_ = 0, total_ = release_.download_size;
    stage_ = Stage::downloading;
    static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    (void)ready;
    thread_ = std::thread([this] { run(); });
}

void Download::run() {
    if (release_.download.empty()) return fail("the release has no download for this computer");
    const fs::path folder = work_ / "download";
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder, ec);
    if (ec) return fail("couldn't make " + folder.string());
    const fs::path file = folder / release_.download, sig = folder / (release_.download + ".sig");
    std::printf("spikeout: downloading %s (%llu bytes)\n", release_.download_url.c_str(), static_cast<unsigned long long>(release_.download_size));
    // the signature (64 bytes), then the download
    std::string signature;
    {
        Sink s;
        s.text = &signature, s.limit = 1024;
        if (std::string why = fetch(release_.signature_url, s, nullptr); !why.empty()) return stop_ ? void(stage_ = Stage::idle) : fail(why);
        std::ofstream(sig, std::ios::binary) << signature;
    }
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        Sink s;
        s.file = &out, s.limit = release_.download_size;
        Progress pr{&done_, &stop_};
        if (std::string why = fetch(release_.download_url, s, &pr); !why.empty()) return stop_ ? void(stage_ = Stage::idle) : fail(why);
        if (!out.flush()) return fail("couldn't write the download (is the drive full?)");
    }
    done_ = release_.download_size;
    stage_ = Stage::checking;
    if (std::string why = check_size(file, release_.download_size); !why.empty()) return fail(why);
    if (!release_.digest.empty())
        if (std::string why = check_digest(file, release_.digest); !why.empty()) return fail(why);
    if (std::string why = check_signature(file, release_.download, sig, public_key()); !why.empty()) return fail(why);
    try {
        unpack(file, staged());
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    std::string said;
    if (std::string why = run_version(staged_program(staged()), folder, said); !why.empty()) return fail("version " + release_.version + " " + why);
    if (said != "spikeout " + release_.version) return fail("the download says it is \"" + said + "\", not version " + release_.version);
    fs::remove(file, ec); // (unpacked: not needed now)
    std::printf("spikeout: version %s downloaded and checked: ready\n", release_.version.c_str());
    stage_ = Stage::ready;
}

} // namespace net::install

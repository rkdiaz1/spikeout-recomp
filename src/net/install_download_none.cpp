// The in-game update's download, in a build without libcurl: never offered.
#include "net/install.hpp"

namespace net::install {

bool Download::available() { return false; }
Download::~Download() = default;
void Download::start(Release release, fs::path work) {
    release_ = std::move(release), work_ = std::move(work);
    error_ = "built without downloads";
    stage_ = Stage::failed;
}
void Download::cancel() {}
std::string Download::error() const { return error_; }
void Download::run() {}
void Download::fail(std::string why) { error_ = std::move(why); }

} // namespace net::install

#pragma once
// A sampling profiler of this program's own code (Windows; elsewhere it does nothing): the thread that made it is
// paused every few microseconds by a thread of the sampler's, where it was noted, and let go. The addresses are kept
// from this program's start, for addr2line -f -C -e PROGRAM. Pausing a thread changes only how long things take.

#include <cstdint>
#include <memory>
#include <string>

namespace m3 {

class HostSampler {
public:
    // Samples the calling thread, or with all_threads every thread of this program but the sampler's (the list
    // looked at again every 20 ms, so threads started later are sampled too; the file says each thread's count).
    explicit HostSampler(unsigned interval_us, bool all_threads = false);
    ~HostSampler();
    HostSampler(const HostSampler&) = delete;
    HostSampler& operator=(const HostSampler&) = delete;
    // Stops sampling and writes "address count" lines, most first; false if it couldn't (or sampling isn't
    // available here). `samples` and `addresses` say how many.
    bool finish(const std::string& file, uint64_t& samples, std::size_t& addresses);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace m3

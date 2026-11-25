#include "runtime/host_sampler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace m3 {

struct HostSampler::State {
    std::atomic<bool> stop{false};
    std::thread thread;
    std::unordered_map<uint64_t, uint64_t> counts;
    uint64_t samples = 0;
    uint64_t base = 0; // (this program's start)
    uint64_t end = 0;  // (and its end: a sample outside it is in a system library, e.g. waiting)
    struct Seen { uint64_t samples = 0, inside = 0; };
    std::map<unsigned long, Seen> threads; // (each thread's samples, and how many were in this program)
#ifdef _WIN32
    std::vector<std::pair<unsigned long, HANDLE>> targets;
#endif
};

#ifdef _WIN32
namespace {
// This program's threads but `skip`, those not already in `targets` opened and added.
void look_for_threads(std::vector<std::pair<unsigned long, HANDLE>>& targets, DWORD skip) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 e{};
    e.dwSize = sizeof e;
    const DWORD me = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &e); ok; ok = Thread32Next(snap, &e)) {
        if (e.th32OwnerProcessID != me || e.th32ThreadID == skip) continue;
        if (std::any_of(targets.begin(), targets.end(), [&](const auto& t) { return t.first == e.th32ThreadID; })) continue;
        if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, e.th32ThreadID))
            targets.emplace_back(e.th32ThreadID, h);
    }
    CloseHandle(snap);
}
} // namespace
#endif

HostSampler::HostSampler(unsigned interval_us, bool all_threads) : state_(std::make_unique<State>()) {
#ifdef _WIN32
    State& s = *state_;
    HMODULE self = GetModuleHandleW(nullptr);
    s.base = reinterpret_cast<uint64_t>(self);
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(self) + dos->e_lfanew);
        s.end = s.base + nt->OptionalHeader.SizeOfImage;
    }
    if (!all_threads) {
        HANDLE h = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
            return;
        s.targets.emplace_back(GetCurrentThreadId(), h);
    }
    s.thread = std::thread([&s, interval_us, all_threads] {
        const DWORD me = GetCurrentThreadId();
        const auto step = std::chrono::microseconds(interval_us);
        auto next = std::chrono::steady_clock::now() + step;
        auto look = std::chrono::steady_clock::now();
        while (!s.stop.load(std::memory_order_relaxed)) {
            while (std::chrono::steady_clock::now() < next) {}
            next += step;
            if (all_threads && std::chrono::steady_clock::now() >= look) {
                look_for_threads(s.targets, me);
                look = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            }
            for (std::size_t i = 0; i < s.targets.size(); ++i) {
                auto [id, h] = s.targets[i];
                if (SuspendThread(h) == static_cast<DWORD>(-1)) { // (it has ended)
                    CloseHandle(h);
                    s.targets.erase(s.targets.begin() + static_cast<std::ptrdiff_t>(i--));
                    continue;
                }
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                const bool got = GetThreadContext(h, &c);
                ResumeThread(h); // (before noting it: noting allocates, and the paused thread may hold the heap's lock)
                if (got) {
                    ++s.counts[c.Rip];
                    ++s.samples;
                    State::Seen& t = s.threads[id];
                    ++t.samples;
                    if (c.Rip >= s.base && c.Rip < s.end) ++t.inside;
                }
            }
        }
    });
#else
    (void)interval_us;
    (void)all_threads;
#endif
}

HostSampler::~HostSampler() {
    state_->stop.store(true, std::memory_order_relaxed);
    if (state_->thread.joinable()) state_->thread.join();
#ifdef _WIN32
    for (auto& t : state_->targets) CloseHandle(t.second);
#endif
}

bool HostSampler::finish(const std::string& file, uint64_t& samples, std::size_t& addresses) {
    State& s = *state_;
    s.stop.store(true, std::memory_order_relaxed);
    if (s.thread.joinable()) s.thread.join();
    samples = s.samples;
    addresses = s.counts.size();
    if (!s.samples) return false;
    std::vector<std::pair<uint64_t, uint64_t>> list(s.counts.begin(), s.counts.end());
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    FILE* f = std::fopen(file.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "# samples %llu, addresses %zu, program at %llx\n", static_cast<unsigned long long>(s.samples), list.size(),
                 static_cast<unsigned long long>(s.base));
    for (const auto& [id, t] : s.threads)
        std::fprintf(f, "# thread %lu samples %llu in this program %llu\n", id, static_cast<unsigned long long>(t.samples),
                     static_cast<unsigned long long>(t.inside));
    for (const auto& [at, n] : list) {
        if (at >= s.base && at < s.end) {
            std::fprintf(f, "%llx %llu\n", static_cast<unsigned long long>(at - s.base), static_cast<unsigned long long>(n));
            continue;
        }
        std::string where = "?"; // (outside this program, e.g. in a system library: its address, then library+offset)
#ifdef _WIN32
        HMODULE m = nullptr;
        char name[MAX_PATH];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(at), &m) &&
            GetModuleFileNameA(m, name, sizeof name)) {
            const char* slash = std::strrchr(name, '\\');
            char off[32];
            std::snprintf(off, sizeof off, "+%llx", static_cast<unsigned long long>(at - reinterpret_cast<uint64_t>(m)));
            where = std::string(slash ? slash + 1 : name) + off;
        }
#endif
        std::fprintf(f, "%llx %llu %s\n", static_cast<unsigned long long>(at), static_cast<unsigned long long>(n), where.c_str());
    }
    std::fclose(f);
    return true;
}

} // namespace m3

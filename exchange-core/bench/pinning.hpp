#pragma once

// Optional thread pinning for the multi-threaded benchmarks. Enabled with
// LOCKSTEP_BENCH_PIN=1; otherwise every call is a no-op, so results from an
// unpinned run are never mistaken for pinned ones (the script records which).

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <pthread.h>
#include <sched.h>

namespace lockstep::bench {

[[nodiscard]] inline bool pinning_enabled() noexcept {
    const char* value = std::getenv("LOCKSTEP_BENCH_PIN");  // NOLINT(concurrency-mt-unsafe)
    return value != nullptr && std::strcmp(value, "1") == 0;
}

namespace detail {

/// CPUs the process may run on, one per physical core: the lowest-numbered
/// SMT sibling of each core (Linux sysfs). Two benchmark threads on sibling
/// hyperthreads would share an L1 cache and report round trips no two real
/// cores can achieve. Falls back to every allowed CPU when sysfs is unreadable.
[[nodiscard]] inline std::vector<std::size_t> probe_cores() {
    std::vector<std::size_t> allowed;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        return allowed;
    }
    for (std::size_t cpu = 0; cpu < static_cast<std::size_t>(CPU_SETSIZE); ++cpu) {
        if (CPU_ISSET(cpu, &set)) {
            allowed.push_back(cpu);
        }
    }
    std::vector<std::size_t> cores;
    for (const std::size_t cpu : allowed) {
        std::ifstream siblings{"/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                               "/topology/thread_siblings_list"};
        std::size_t lowest = cpu;
        if (siblings && (siblings >> lowest) && lowest != cpu) {
            continue;  // a lower-numbered sibling represents this core
        }
        cores.push_back(cpu);
    }
    return cores.empty() ? allowed : cores;
}

// Computed once, at program start, from the affinity the process was launched
// with. Probing later would read the mask of a thread that an earlier
// benchmark already pinned (new threads inherit their creator's mask) and
// collapse every thread onto one CPU.
// An allocation failure this early would terminate the process, which is the
// right outcome for a benchmark binary.
// NOLINTNEXTLINE(cert-err58-cpp)
inline const std::vector<std::size_t> startup_cores = probe_cores();

}  // namespace detail

/// Pins the calling thread to the `slot`-th physical core and restores the
/// previous affinity on destruction, so one benchmark's pin never leaks into
/// the next one or into threads created later. A no-op unless
/// LOCKSTEP_BENCH_PIN=1, and also for a slot beyond the number of physical
/// cores: such a thread floats rather than sharing a core with a spinning
/// peer, which would measure the scheduler.
class ScopedPin {
public:
    explicit ScopedPin(std::size_t slot) {
        if (!pinning_enabled() || slot >= detail::startup_cores.size()) {
            return;
        }
        if (pthread_getaffinity_np(pthread_self(), sizeof(previous_), &previous_) != 0) {
            return;
        }
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(detail::startup_cores[slot], &set);
        active_ = pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
    }
    ScopedPin(const ScopedPin&) = delete;
    ScopedPin& operator=(const ScopedPin&) = delete;
    ScopedPin(ScopedPin&&) = delete;
    ScopedPin& operator=(ScopedPin&&) = delete;
    ~ScopedPin() {
        if (active_) {
            pthread_setaffinity_np(pthread_self(), sizeof(previous_), &previous_);
        }
    }

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    cpu_set_t previous_{};
    bool active_{false};
};

}  // namespace lockstep::bench

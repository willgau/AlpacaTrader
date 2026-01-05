#pragma once

#include "common.h"
#include "log2histogram.hpp"
#include "myboost.h"

static inline std::uint64_t qpc_now() 
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<std::uint64_t>(t.QuadPart);
}
static inline std::uint64_t qpc_freq() 
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<std::uint64_t>(f.QuadPart);
}
static void pin_current_thread_to_cpu(unsigned cpu_index) 
{
    DWORD_PTR mask = (DWORD_PTR(1) << cpu_index);
    SetThreadAffinityMask(GetCurrentThread(), mask);
}

static inline std::uint64_t ticks_to_ns(std::uint64_t ticks, std::uint64_t freq) {
    // ns = ticks * 1e9 / freq
    long double ns = (long double)ticks * 1000000000.0L / (long double)freq;
    return (std::uint64_t)ns;
}

struct BenchConfig {
    std::uint64_t messages = 5'000'000;
    std::uint32_t sample_every = 1;             // record latency every N messages (1 = all)
    std::chrono::microseconds empty_backoff{ 50 }; // async sleep when queue empty
};

struct BenchResults {
    std::uint64_t consumed = 0;
    std::uint64_t checksum = 0;

    std::uint64_t min_ns = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_ns = 0;
    double avg_ns = 0.0;

    Log2Histogram hist;
};

struct TimingState 
{
    std::chrono::steady_clock::time_point start{};
    std::chrono::steady_clock::time_point end{};
};

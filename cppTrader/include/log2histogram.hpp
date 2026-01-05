#include "common.h"

struct Log2Histogram {
    static constexpr int BINS = 64;
    std::array<std::uint64_t, BINS> count{};
    std::uint64_t total = 0;

    static int bin_index(std::uint64_t ns) {
        if (ns == 0) return 0;

        unsigned long idx;
        _BitScanReverse64(&idx, ns);
        int i = (int)idx;
        return (i < BINS - 1) ? i : (BINS - 1);
    }

    void add(std::uint64_t ns) {
        ++count[bin_index(ns)];
        ++total;
    }

    std::uint64_t percentile(double p) const {
        if (total == 0) return 0;
        std::uint64_t target = (std::uint64_t)(p * (double)total);
        if (target == 0) target = 1;

        std::uint64_t c = 0;
        for (int i = 0; i < BINS; ++i) {
            c += count[i];
            if (c >= target) {
                return (i >= 62) ? std::numeric_limits<std::uint64_t>::max() : (1ull << (i + 1));
            }
        }
        return std::numeric_limits<std::uint64_t>::max();
    }
};
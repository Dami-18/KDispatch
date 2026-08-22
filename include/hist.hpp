// Log-bucketed latency histogram.
//
// Same shape as HdrHistogram: buckets are grouped by power-of-two octave with
// SUB linear sub-buckets inside each, giving constant relative precision
// (1/SUB ~= 3%) across the whole range. Recording is a few ALU ops and one
// array increment, so it is safe to call on the measurement path.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace kd {

class Histogram {
public:
    static constexpr int SUB_BITS = 5;
    static constexpr int SUB = 1 << SUB_BITS;   // 32 sub-buckets per octave
    static constexpr int NBUCKETS = 2048;       // covers well past 1e18 ns

    void record(std::uint64_t v) {
        ++count_;
        sum_ += v;
        min_ = std::min(min_, v);
        max_ = std::max(max_, v);
        buckets_[index_of(v)] += 1;
    }

    void merge(const Histogram& o) {
        count_ += o.count_;
        sum_ += o.sum_;
        min_ = std::min(min_, o.min_);
        max_ = std::max(max_, o.max_);
        for (int i = 0; i < NBUCKETS; ++i) buckets_[i] += o.buckets_[i];
    }

    std::uint64_t count() const { return count_; }
    std::uint64_t min() const { return count_ ? min_ : 0; }
    std::uint64_t max() const { return max_; }
    double mean() const { return count_ ? static_cast<double>(sum_) / count_ : 0.0; }

    // p in [0,1]. Returns the lower bound of the containing bucket, i.e. a
    // slightly conservative (never inflated) estimate.
    std::uint64_t percentile(double p) const {
        if (count_ == 0) return 0;
        const std::uint64_t target =
            static_cast<std::uint64_t>(p * static_cast<double>(count_) + 0.5);
        std::uint64_t seen = 0;
        for (int i = 0; i < NBUCKETS; ++i) {
            seen += buckets_[i];
            if (seen >= target) return value_of(i);
        }
        return max_;
    }

private:
    static int index_of(std::uint64_t v) {
        if (v < static_cast<std::uint64_t>(SUB)) return static_cast<int>(v);
        const int msb = 63 - __builtin_clzll(v);
        const int shift = msb - SUB_BITS;
        const int sub = static_cast<int>(v >> shift) - SUB;
        const int idx = sub + (shift + 1) * SUB;
        return idx < NBUCKETS ? idx : NBUCKETS - 1;
    }

    static std::uint64_t value_of(int idx) {
        if (idx < SUB) return static_cast<std::uint64_t>(idx);
        const int shift = idx / SUB - 1;
        const int sub = idx % SUB;
        return static_cast<std::uint64_t>(sub + SUB) << shift;
    }

    std::array<std::uint64_t, NBUCKETS> buckets_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = ~0ull;
    std::uint64_t max_ = 0;
};

}  // namespace kd

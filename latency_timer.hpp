#pragma once

#include <chrono>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>

namespace hft {

/**
 * @brief High-resolution latency recorder with percentile reporting.
 *
 * Samples are stored in a pre-allocated vector to avoid any allocations
 * during the measurement window — allocation itself would pollute the
 * latency distribution.
 */
class LatencyTimer {
public:
    using Clock    = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Nanos    = int64_t;

    explicit LatencyTimer(std::size_t reserve_samples = 1'000'000)
        : label_("unnamed") {
        samples_.reserve(reserve_samples);
    }

    explicit LatencyTimer(std::string label, std::size_t reserve_samples = 1'000'000)
        : label_(std::move(label)) {
        samples_.reserve(reserve_samples);
    }

    [[nodiscard]] TimePoint start() const noexcept {
        return Clock::now();
    }

    void record(TimePoint t0) noexcept {
        auto t1 = Clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        samples_.push_back(static_cast<Nanos>(ns));
    }

    void record_ns(Nanos ns) noexcept {
        samples_.push_back(ns);
    }

    void reset() noexcept { samples_.clear(); }

    [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }

    struct Stats {
        Nanos   min_ns    = 0;
        Nanos   max_ns    = 0;
        double  mean_ns   = 0.0;
        double  stddev_ns = 0.0;
        Nanos   p50_ns    = 0;
        Nanos   p90_ns    = 0;
        Nanos   p99_ns    = 0;
        Nanos   p999_ns   = 0;
        std::size_t samples = 0;
    };

    [[nodiscard]] Stats compute() const {
        if (samples_.empty()) return {};

        std::vector<Nanos> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());

        Stats s;
        s.samples   = sorted.size();
        s.min_ns    = sorted.front();
        s.max_ns    = sorted.back();

        double sum  = std::accumulate(sorted.begin(), sorted.end(), 0.0);
        s.mean_ns   = sum / static_cast<double>(s.samples);

        double sq_sum = 0.0;
        for (auto v : sorted) {
            double diff = static_cast<double>(v) - s.mean_ns;
            sq_sum += diff * diff;
        }
        s.stddev_ns = std::sqrt(sq_sum / static_cast<double>(s.samples));

        auto pct = [&](double p) -> Nanos {
            std::size_t idx = static_cast<std::size_t>(
                std::ceil(p * static_cast<double>(s.samples))) - 1;
            idx = std::min(idx, s.samples - 1);
            return sorted[idx];
        };

        s.p50_ns  = pct(0.500);
        s.p90_ns  = pct(0.900);
        s.p99_ns  = pct(0.990);
        s.p999_ns = pct(0.999);

        return s;
    }

    [[nodiscard]] std::string report() const {
        auto s = compute();
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        oss << "=== Latency Report: " << label_ << " ===\n";
        oss << "  Samples : " << s.samples << "\n";
        oss << "  Min     : " << s.min_ns    << " ns\n";
        oss << "  Mean    : " << s.mean_ns   << " ns\n";
        oss << "  Std Dev : " << s.stddev_ns << " ns\n";
        oss << "  p50     : " << s.p50_ns    << " ns\n";
        oss << "  p90     : " << s.p90_ns    << " ns\n";
        oss << "  p99     : " << s.p99_ns    << " ns\n";
        oss << "  p99.9   : " << s.p999_ns   << " ns\n";
        oss << "  Max     : " << s.max_ns    << " ns\n";
        return oss.str();
    }

private:
    std::string        label_;
    std::vector<Nanos> samples_;
};

} // namespace hft
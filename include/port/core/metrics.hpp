#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace port::core {

// Simple metrics registry: counters, gauges, and latency histograms.
class Metrics {
public:
    static Metrics& instance();

    // Increment a named counter by delta (default 1)
    void counter(const std::string& name, int64_t delta = 1);

    // Set a gauge to an exact value
    void gauge(const std::string& name, double value);

    // Record a latency sample (milliseconds)
    void latency(const std::string& name, double ms);

    // RAII timer: records elapsed time into name on destruction
    struct Timer {
        explicit Timer(std::string name);
        ~Timer();
    private:
        std::string name_;
        std::chrono::steady_clock::time_point start_;
    };
    [[nodiscard]] Timer time(std::string name);

    struct Snapshot {
        std::unordered_map<std::string, int64_t> counters;
        std::unordered_map<std::string, double>  gauges;
        struct LatencyStats { double p50, p95, p99, max; };
        std::unordered_map<std::string, LatencyStats> latencies;
    };
    [[nodiscard]] Snapshot snapshot() const;

    void reset();

private:
    Metrics() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::atomic<int64_t>*> counters_;
    std::unordered_map<std::string, double> gauges_;
    std::unordered_map<std::string, std::vector<double>> latencyBuckets_;
};

} // namespace port::core

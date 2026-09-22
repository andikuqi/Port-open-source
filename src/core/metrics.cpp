#include "port/core/metrics.hpp"

#include <algorithm>
#include <new>

namespace port::core {

Metrics& Metrics::instance() {
    static Metrics inst;
    return inst;
}

void Metrics::counter(const std::string& name, int64_t delta) {
    std::lock_guard lock{mutex_};
    auto it = counters_.find(name);
    if (it == counters_.end()) {
        auto* p = new std::atomic<int64_t>{delta};
        counters_[name] = p;
    } else {
        it->second->fetch_add(delta, std::memory_order_relaxed);
    }
}

void Metrics::gauge(const std::string& name, double value) {
    std::lock_guard lock{mutex_};
    gauges_[name] = value;
}

void Metrics::latency(const std::string& name, double ms) {
    std::lock_guard lock{mutex_};
    latencyBuckets_[name].push_back(ms);
}

Metrics::Timer::Timer(std::string name)
    : name_{std::move(name)}, start_{std::chrono::steady_clock::now()} {}

Metrics::Timer::~Timer() {
    const auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start_).count();
    Metrics::instance().latency(name_, ms);
}

Metrics::Timer Metrics::time(std::string name) {
    return Timer{std::move(name)};
}

Metrics::Snapshot Metrics::snapshot() const {
    std::lock_guard lock{mutex_};
    Snapshot snap;

    for (const auto& [name, ptr] : counters_) {
        snap.counters[name] = ptr->load(std::memory_order_relaxed);
    }
    snap.gauges = gauges_;

    for (const auto& [name, samples] : latencyBuckets_) {
        if (samples.empty()) continue;
        auto sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        const auto pct = [&](double p) {
            const std::size_t idx = static_cast<std::size_t>(p * (sorted.size() - 1));
            return sorted[idx];
        };
        snap.latencies[name] = {pct(0.50), pct(0.95), pct(0.99), sorted.back()};
    }
    return snap;
}

void Metrics::reset() {
    std::lock_guard lock{mutex_};
    for (auto& [k, p] : counters_) { delete p; }
    counters_.clear();
    gauges_.clear();
    latencyBuckets_.clear();
}

} // namespace port::core

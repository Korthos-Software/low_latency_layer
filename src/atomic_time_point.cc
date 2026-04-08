#include "atomic_time_point.hh"

#include <cassert>

namespace low_latency {

AtomicTimePoint::AtomicTimePoint() {}

AtomicTimePoint::~AtomicTimePoint() {}

bool AtomicTimePoint::has_value() const {
    return this->count.load(std::memory_order_relaxed);
}

std::chrono::steady_clock::time_point AtomicTimePoint::get() const {
    const auto result = this->count.load(std::memory_order_relaxed);
    assert(result);
    using namespace std::chrono;
    return steady_clock::time_point{steady_clock::duration{result}};
}

void AtomicTimePoint::set(const std::chrono::steady_clock::time_point target) {
    this->count.store(target.time_since_epoch().count(),
                      std::memory_order_relaxed);
}

} // namespace low_latency
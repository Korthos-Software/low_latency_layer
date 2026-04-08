#ifndef ATOMIC_TIME_POINT_HH_
#define ATOMIC_TIME_POINT_HH_

#include <atomic>
#include <chrono>

// The purpose of this class is to provide a simple time point which may be read
// from atomically and without locks.

namespace low_latency {

class AtomicTimePoint final {
  private:
    std::atomic<std::int64_t> count{};
    static_assert(decltype(count)::is_always_lock_free);

  public:
    AtomicTimePoint();
    AtomicTimePoint(const AtomicTimePoint&) = delete;
    AtomicTimePoint(AtomicTimePoint&&) = delete;
    AtomicTimePoint operator=(const AtomicTimePoint&) = delete;
    AtomicTimePoint operator=(AtomicTimePoint&&) = delete;
    ~AtomicTimePoint();

  public:
    bool has_value() const;

    std::chrono::steady_clock::time_point get() const;

    void set(const std::chrono::steady_clock::time_point target);
};

} // namespace low_latency

#endif
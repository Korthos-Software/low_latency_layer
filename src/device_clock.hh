#ifndef CLOCK_HH_
#define CLOCK_HH_

#include <chrono>

// This header provides a DeviceClock that abstracts away the Vulkan details of
// comparing CPU and GPU times.

namespace low_latency {

class DeviceContext;

class DeviceClock final {
  public:
    // FIXME this is bad, see now().
    using time_point_t = std::chrono::time_point<std::chrono::steady_clock,
                                                 std::chrono::nanoseconds>;
    const DeviceContext& device;

  public:
    std::uint64_t host_ns;
    std::uint64_t error_bound;
    std::uint64_t device_ticks;

  public:
    DeviceClock(const DeviceContext& device);
    DeviceClock(const DeviceClock&) = delete;
    DeviceClock(DeviceClock&&) = delete;
    DeviceClock operator=(const DeviceClock&) = delete;
    DeviceClock operator=(DeviceClock&&) = delete;
    ~DeviceClock();

  public:
    // WARNING: This *MUST* be used over std::chrono::steady_clock::now if
    // you're planning on comparing it to a device's clock. If it isn't, the
    // timestamps might from different domains and will be completely
    // nonsensical.
    // FIXME we should be able to fix this with a tiny wrapper class of
    // time_point_t that enforces typesafety.
    static time_point_t now();

  public:
    void calibrate();
    time_point_t ticks_to_time(const std::uint64_t& ticks) const;
};

} // namespace low_latency

#endif
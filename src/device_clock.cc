#include "device_clock.hh"
#include "device_context.hh"
#include "helper.hh"

#include <vulkan/vulkan_core.h>

#include <cassert>
#include <time.h>

namespace low_latency {

DeviceClock::DeviceClock(const DeviceContext& context) : device(context) {
    this->calibrate();
}

DeviceClock::~DeviceClock() {}

DeviceClock::time_point_t DeviceClock::now() {
    auto ts = timespec{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        throw errno;
    }

    return time_point_t{std::chrono::seconds{ts.tv_sec} +
                        std::chrono::nanoseconds{ts.tv_nsec}};
}

void DeviceClock::calibrate() {
    const auto infos = std::vector<VkCalibratedTimestampInfoKHR>{
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr,
         VK_TIME_DOMAIN_DEVICE_EXT},
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr,
         VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT}};

    struct CalibratedResult {
        std::uint64_t device;
        std::uint64_t host;
    };
    auto calibrated_result = CalibratedResult{};

    THROW_NOT_VKSUCCESS(device.vtable.GetCalibratedTimestampsKHR(
        device.device, 2, std::data(infos), &calibrated_result.device,
        &this->error_bound));

    this->device_ticks = calibrated_result.device;
    this->host_ns = calibrated_result.host;
}

DeviceClock::time_point_t
DeviceClock::ticks_to_time(const std::uint64_t& ticks) const {
    const auto& pd = device.physical_device.properties;
    const auto ns_tick = static_cast<double>(pd->limits.timestampPeriod);

    const auto diff = [&]() -> auto {
        auto a = this->device_ticks;
        auto b = ticks;
        const auto is_negative = a > b;
        if (is_negative) {
            std::swap(a, b);
        }
        const auto abs_diff = b - a;
        assert(abs_diff <= std::numeric_limits<std::int64_t>::max());
        const auto signed_abs_diff = static_cast<std::int64_t>(abs_diff);
        return is_negative ? -signed_abs_diff : signed_abs_diff;
    }();

    const auto diff_nsec =
        static_cast<std::int64_t>(static_cast<double>(diff) * ns_tick + 0.5);
    const auto delta_ns = static_cast<std::int64_t>(this->host_ns) + diff_nsec;
    return time_point_t{std::chrono::nanoseconds(delta_ns)};
}

} // namespace low_latency
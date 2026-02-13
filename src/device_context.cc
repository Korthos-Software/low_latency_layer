#include "device_context.hh"
#include "queue_context.hh"

#include <utility>

namespace low_latency {

DeviceContext::DeviceContext(InstanceContext& parent_instance,
                             PhysicalDeviceContext& parent_physical_device,
                             const VkDevice& device,
                             const PFN_vkSetDeviceLoaderData& sdld,
                             VkuDeviceDispatchTable&& vtable)
    : instance(parent_instance), physical_device(parent_physical_device),
      device(device), sdld(sdld), vtable(std::move(vtable)), clock(*this) {}

DeviceContext::~DeviceContext() {
    // We will let the destructor handle clearing here, but they should be
    // unique by now (ie, removed from the layer's context map).
    for (const auto& [queue, queue_context] : this->queues) {
        assert(queue_context.unique());
    }
}

void DeviceContext::notify_acquire(const VkSwapchainKHR& swapchain,
                                   const std::uint32_t& image_index,
                                   const VkSemaphore& signal_semaphore) {

    const auto it = this->swapchain_signals.try_emplace(swapchain).first;

    // Doesn't matter if it was already there, overwrite it.
    it->second.insert_or_assign(image_index, signal_semaphore);
}

DeviceContext::Clock::Clock(const DeviceContext& context) {

    const auto infos = std::vector<VkCalibratedTimestampInfoKHR>{
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr,
         VK_TIME_DOMAIN_DEVICE_EXT},
        {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr,
         VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT}};

    auto device_host = std::array<std::uint64_t, 2>{};

    const auto steady_before = std::chrono::steady_clock::now();
    context.vtable.GetCalibratedTimestampsKHR(
        context.device, 2, std::data(infos), std::data(device_host),
        &this->error_bound);
    const auto steady_after = std::chrono::steady_clock::now();

    this->cpu_time = steady_before + (steady_after - steady_before) / 2;
    this->device_ticks = device_host[0];
    this->host_ns = device_host[1];

    // Might need to get physical limits again?
    this->ticks_per_ns =
        context.physical_device.properties->limits.timestampPeriod;
}

DeviceContext::Clock::time_point_t
DeviceContext::Clock::ticks_to_time(const std::uint64_t& ticks) const {
    /*
    struct timespec tv;
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return tv.tv_nsec + tv.tv_sec*1000000000ull;
    */

    auto a = this->device_ticks;
    auto b = ticks;

    const auto was_before = a > b;
    if (was_before) { // it's happened before
        std::swap(a, b);
    }
    const auto nsec = std::chrono::nanoseconds((b - a) * this->ticks_per_ns);
    return this->cpu_time + (was_before ? -nsec : nsec);
}

void DeviceContext::calibrate_timestamps() { this->clock = Clock{*this}; }

} // namespace low_latency
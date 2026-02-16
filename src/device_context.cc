#include "device_context.hh"
#include "queue_context.hh"

#include <iostream>
#include <utility>

namespace low_latency {

DeviceContext::DeviceContext(InstanceContext& parent_instance,
                             PhysicalDeviceContext& parent_physical_device,
                             const VkDevice& device,
                             VkuDeviceDispatchTable&& vtable)
    : instance(parent_instance), physical_device(parent_physical_device),
      device(device), vtable(std::move(vtable)), clock(*this) {}

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

    std::cerr << "notify acquire for swapchain: " << swapchain << " : "
              << image_index << '\n';
    std::cerr << "    signal semaphore: " << signal_semaphore << '\n';

    const auto it = this->swapchain_signals.try_emplace(swapchain).first;

    // Doesn't matter if it was already there, overwrite it.
    it->second.insert_or_assign(image_index, signal_semaphore);
}

DeviceContext::Clock::Clock(const DeviceContext& context) : device(context) {
    this->calibrate();
}

DeviceContext::Clock::~Clock() {}

void DeviceContext::Clock::calibrate() {
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

    // we probably want to use this instead bc clock_gettime isn't guaranteed
    // by steady clock afaik
    /*
    struct timespec tv;
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return tv.tv_nsec + tv.tv_sec*1000000000ull;
    */

    const auto steady_before = std::chrono::steady_clock::now();
    device.vtable.GetCalibratedTimestampsKHR(device.device, 2, std::data(infos),
                                             &calibrated_result.device,
                                             &this->error_bound);
    const auto steady_after = std::chrono::steady_clock::now();

    this->cpu_time = steady_before + (steady_after - steady_before) / 2;
    this->device_ticks = calibrated_result.device;
    this->host_ns = calibrated_result.host;

    // Might need to get physical limits every now and then?
    const auto& pd = device.physical_device.properties;
    this->ticks_per_ns = pd->limits.timestampPeriod;
}

DeviceContext::Clock::time_point_t
DeviceContext::Clock::ticks_to_time(const std::uint64_t& ticks) const {
    auto a = this->device_ticks;
    auto b = ticks;
    const auto was_before = a > b;
    if (was_before) { // it's happened before
        std::swap(a, b);
    }

    const auto nsec = std::chrono::nanoseconds((b - a) * this->ticks_per_ns);
    return this->cpu_time + (was_before ? -nsec : nsec);
}

} // namespace low_latency
#include "device_context.hh"
#include "queue_context.hh"

#include <iostream>
#include <utility>
#include <vulkan/vulkan_core.h>

namespace low_latency {

DeviceContext::DeviceContext(InstanceContext& parent_instance,
                             PhysicalDeviceContext& parent_physical_device,
                             const VkDevice& device,
                             VkuDeviceDispatchTable&& vtable)
    : instance(parent_instance), physical_device(parent_physical_device),
      device(device), vtable(std::move(vtable)), clock(*this) {}

DeviceContext::~DeviceContext() {
    this->present_queue.reset();
    // We will let the destructor handle clearing here, but they should be
    // unique by now (ie, removed from the layer's context map).
    for (const auto& [queue, queue_context] : this->queues) {
        assert(queue_context.unique());
    }
}

void DeviceContext::notify_acquire(const VkSwapchainKHR& swapchain,
                                   const std::uint32_t& image_index,
                                   const VkSemaphore& signal_semaphore) {

    /*
    std::cerr << "notify acquire for swapchain: " << swapchain << " : "
              << image_index << '\n';
    std::cerr << "    signal semaphore: " << signal_semaphore << '\n';
    */

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
    device.vtable.GetCalibratedTimestampsKHR(device.device, 2, std::data(infos),
                                             &calibrated_result.device,
                                             &this->error_bound);
    this->device_ticks = calibrated_result.device;
    this->host_ns = calibrated_result.host;
}

DeviceContext::Clock::time_point_t
DeviceContext::Clock::ticks_to_time(const std::uint64_t& ticks) const {
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

    // This will have issues because std::chrono::steady_clock::now(), which
    // we use for cpu time, may not be on the same time domain what was returned
    // by GetCalibratedTimestamps. It would be more robust to use the posix
    // gettime that vulkan guarantees it can be compared to instead.

    const auto diff_nsec = static_cast<std::int64_t>(diff * ns_tick + 0.5);
    const auto delta = std::chrono::nanoseconds(this->host_ns + diff_nsec);
    return time_point_t{delta};
}

const auto debug_log_time2 = [](auto& stream, const auto& diff) {
    using namespace std::chrono;
    const auto ms = duration_cast<milliseconds>(diff);
    const auto us = duration_cast<microseconds>(diff - ms);
    const auto ns = duration_cast<nanoseconds>(diff - ms - us);
    stream << ms << " " << us << " " << ns << '\n';
};

const auto debug_log_time = [](const auto& diff) {
    debug_log_time2(std::cerr, diff);
};

void DeviceContext::sleep_in_input() {
    // Present hasn't happened yet, we don't know what queue to attack.
    if (!this->present_queue) {
        return;
    }

    const auto before = std::chrono::steady_clock::now();
    // If we're here, that means that there might be an outstanding frame that's
    // sitting on our present_queue which hasn't yet completed, so we need to
    // stall until it's finished.
    const auto& frames = this->present_queue->in_flight_frames;
    if (std::size(frames)) {
        frames.back().submissions.back()->end_handle->get_time_spinlock();
    }
    const auto after = std::chrono::steady_clock::now();
    //debug_log_time(after - before);
    
    // FIXME this should take into account 'cpu_time', which we currently do not...
    // idk if it matters.
}

void DeviceContext::notify_antilag_update(const VkAntiLagDataAMD& data) {
    this->antilag_mode = data.mode;
    this->antilag_fps = data.maxFPS;

    // This might not be provided (probably just to set some settings).
    if (!data.pPresentationInfo) {
        return;
    }

    const auto& presentation_info = *data.pPresentationInfo;
    // Only care about the input stage for now.
    if (presentation_info.stage != VK_ANTI_LAG_STAGE_INPUT_AMD) {
        return;
    }

    if (this->antilag_mode == VK_ANTI_LAG_MODE_ON_AMD) {
        this->sleep_in_input();
    }
}

void DeviceContext::notify_queue_present(const QueueContext& queue) {
    assert(this->queues.contains(queue.queue));
    this->present_queue = this->queues[queue.queue];
}

} // namespace low_latency
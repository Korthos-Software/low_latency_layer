#include "device_context.hh"
#include "queue_context.hh"

#include <utility>
#include <vulkan/vulkan_core.h>

namespace low_latency {

DeviceContext::DeviceContext(InstanceContext& parent_instance,
                             PhysicalDeviceContext& parent_physical_device,
                             const VkDevice& device,
                             const bool was_antilag_requested,
                             VkuDeviceDispatchTable&& vtable)
    : instance(parent_instance), physical_device(parent_physical_device),
      device(device), was_antilag_requested(was_antilag_requested),
      vtable(std::move(vtable)) {

    // Only create our clock if we can support creating it.
    if (this->physical_device.supports_required_extensions) {
        this->clock = std::make_unique<Clock>(*this);
    }
}

DeviceContext::~DeviceContext() {
    this->present_queue.reset();
    // We will let the destructor handle clearing here, but they should be
    // unique by now (ie, removed from the layer's context map).
    for (const auto& [queue, queue_context] : this->queues) {
        assert(queue_context.unique());
    }
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

void DeviceContext::sleep_in_input() {
    // Present hasn't happened yet, we don't know what queue to attack.
    if (!this->present_queue) {
        return;
    }

    const auto& frames = this->present_queue->in_flight_frames;
    // No frame here means we're behind the GPU and do not need to delay.
    // If anything we should speed up...
    if (!std::size(frames)) {
        return;
    }

    // If we're here, that means that there might be an outstanding frame that's
    // sitting on our present_queue which hasn't yet completed, so we need to
    // stall until it's finished.
    const auto& last_frame = frames.back();
    assert(std::size(last_frame.submissions));
    const auto& last_frame_submission = last_frame.submissions.back();
    last_frame_submission->end_handle->get_time_spinlock();

    // From our sleep in present implementation, just spinning until
    // the previous frame has completed did not work well. This was because
    // there was a delay between presentation and when new work was given
    // to the GPU. If we stalled the CPU without trying to account for this, we
    // would get huge frame drops, loss of throughput, and the GPU would even
    // clock down. So naturally I am concerned about this approach, but it seems
    // to perform well so far in my own testing and is just beautifully elegant.
}

void DeviceContext::notify_antilag_update(const VkAntiLagDataAMD& data) {
    this->antilag_mode = data.mode;
    this->antilag_fps = data.maxFPS; // TODO

    // This might not be provided (probably just to set some settings?).
    if (!data.pPresentationInfo) {
        return;
    }

    // Only care about the input stage for now.
    if (data.pPresentationInfo->stage != VK_ANTI_LAG_STAGE_INPUT_AMD) {
        return;
    }

    if (this->antilag_mode != VK_ANTI_LAG_MODE_ON_AMD) {
        return;
    }

    this->sleep_in_input();
}

void DeviceContext::notify_queue_present(const QueueContext& queue) {
    assert(this->queues.contains(queue.queue));
    this->present_queue = this->queues[queue.queue];
}

} // namespace low_latency
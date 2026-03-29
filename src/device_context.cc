#include "device_context.hh"

#include <utility>
#include <vulkan/vulkan_core.h>

namespace low_latency {

DeviceContext::DeviceContext(InstanceContext& parent_instance,
                             PhysicalDeviceContext& parent_physical_device,
                             const VkDevice& device,
                             const bool was_capability_requested,
                             VkuDeviceDispatchTable&& vtable)
    : instance(parent_instance), physical_device(parent_physical_device),
      was_capability_requested(was_capability_requested), device(device),
      vtable(std::move(vtable)) {

    // Only create our clock if we can support creating it.
    if (this->physical_device.supports_required_extensions) {
        this->clock = std::make_unique<DeviceClock>(*this);
    }
}

DeviceContext::~DeviceContext() {
    // We will let the destructor handle clearing here, but they should be
    // unique by now (ie, removed from the layer's context map).
    for (const auto& [queue, queue_context] : this->queues) {
        assert(queue_context.unique());
    }
}

/*
void DeviceContext::sleep_in_input() {
    // TODO

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
*/

void DeviceContext::update_params(
    const std::optional<VkSwapchainKHR> target,
    const std::chrono::milliseconds& present_delay,
    const bool was_low_latency_requested) {

    // If we don't have a target (AMD's anti_lag doesn't differentiate between
    // swapchains), just write it to everything.
    if (!target.has_value()) {
        for (auto& iter : this->swapchain_monitors) {
            iter.second.update_params(was_low_latency_requested, present_delay);
        }
        return;
    }

    const auto iter = this->swapchain_monitors.find(*target);
    assert(iter != std::end(this->swapchain_monitors));
    iter->second.update_params(was_low_latency_requested, present_delay);
}

void DeviceContext::notify_present(
    const VkSwapchainKHR& swapchain,
    const QueueContext::submissions_t& submissions) {

    const auto iter = this->swapchain_monitors.find(swapchain);
    assert(iter != std::end(this->swapchain_monitors));

    iter->second.notify_present(submissions);
}

} // namespace low_latency
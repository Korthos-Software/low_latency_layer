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

    // Only create our clock if we were asked to do anything.
    if (this->was_capability_requested) {
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

void DeviceContext::update_params(
    const std::optional<VkSwapchainKHR> target,
    const std::chrono::milliseconds& present_delay,
    const bool was_low_latency_requested) {

    // If we don't have a target (AMD's anti_lag doesn't differentiate between
    // swapchains) just write it to everything.
    if (!target.has_value()) {
        for (auto& iter : this->swapchain_monitors) {
            iter.second->update_params(was_low_latency_requested,
                                       present_delay);
        }
        return;
    }

    const auto iter = this->swapchain_monitors.find(*target);
    assert(iter != std::end(this->swapchain_monitors));
    iter->second->update_params(was_low_latency_requested, present_delay);
}

void DeviceContext::notify_present(
    const VkSwapchainKHR& swapchain,
    std::unique_ptr<QueueContext::Submissions> submissions) {

    const auto iter = this->swapchain_monitors.find(swapchain);
    assert(iter != std::end(this->swapchain_monitors));

    iter->second->notify_present(std::move(submissions));
}

} // namespace low_latency
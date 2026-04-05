#include "device_context.hh"

#include "layer_context.hh"
#include "strategies/anti_lag/device_strategy.hh"
#include "strategies/low_latency2/device_strategy.hh"

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
    if (!this->was_capability_requested) {
        return;
    }

    this->clock = std::make_unique<DeviceClock>(*this);
    this->strategy = [&]() -> std::unique_ptr<DeviceStrategy> {
        if (parent_instance.layer.should_expose_reflex) {
            return std::make_unique<LowLatency2DeviceStrategy>(*this);
        }
        return std::make_unique<AntiLagDeviceStrategy>(*this);
    }();
}

DeviceContext::~DeviceContext() {
    // We will let the destructor handle clearing here, but they should be
    // unique by now (ie, removed from the layer's context map).
    for (const auto& [queue, queue_context] : this->queues) {
        assert(queue_context.unique());
    }
}

} // namespace low_latency
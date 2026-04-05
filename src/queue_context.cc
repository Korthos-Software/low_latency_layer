#include "queue_context.hh"
#include "device_context.hh"
#include "helper.hh"
#include "layer_context.hh"
#include "strategies/anti_lag/queue_strategy.hh"
#include "strategies/low_latency2/queue_strategy.hh"
#include "timestamp_pool.hh"

#include <span>

#include <vulkan/vulkan_core.h>

namespace low_latency {

QueueContext::CommandPoolOwner::CommandPoolOwner(const QueueContext& queue)
    : queue(queue) {

    const auto& device_context = this->queue.device;

    const auto cpci = VkCommandPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue.queue_family_index,
    };

    THROW_NOT_VKSUCCESS(device_context.vtable.CreateCommandPool(
        device_context.device, &cpci, nullptr, &this->command_pool));
}

QueueContext::CommandPoolOwner::~CommandPoolOwner() {
    const auto& device_context = this->queue.device;
    device_context.vtable.DestroyCommandPool(device_context.device,
                                             this->command_pool, nullptr);
}

QueueContext::QueueContext(DeviceContext& device, const VkQueue& queue,
                           const std::uint32_t& queue_family_index)
    : device(device), queue(queue), queue_family_index(queue_family_index),
      command_pool(std::make_unique<CommandPoolOwner>(*this)) {

    // Only construct things if we actually support our operations.
    if (!device.physical_device.supports_required_extensions) {
        return;
    }

    this->timestamp_pool = std::make_unique<TimestampPool>(*this);
    this->strategy = [&]() -> std::unique_ptr<QueueStrategy> {
        if (device.instance.layer.should_expose_reflex) {
            return std::make_unique<LowLatency2QueueStrategy>(*this);
        }
        return std::make_unique<AntiLagQueueStrategy>(*this);
    }();
}

QueueContext::~QueueContext() { this->timestamp_pool.reset(); }

bool QueueContext::should_inject_timestamps() const {
    const auto& physical_device = this->device.physical_device;

    // Our layer is a no-op here if we don't support it.
    if (!physical_device.supports_required_extensions) {
        return false;
    }

    // Don't bother injecting timestamps during queue submission if we
    // aren't planning on doing anything anyway.
    if (!this->device.was_capability_requested) {
        return false;
    }

    assert(physical_device.queue_properties);
    const auto& queue_props = *physical_device.queue_properties;
    assert(this->queue_family_index < std::size(queue_props));

    const auto& props = queue_props[this->queue_family_index];
    // Probably need at least 64, don't worry about it just yet and just ensure
    // it's not zero (because that will cause a crash if we inject).
    return props.queueFamilyProperties.timestampValidBits;
}

} // namespace low_latency
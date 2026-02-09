#ifndef DEVICE_CONTEXT_HH_
#define DEVICE_CONTEXT_HH_

#include <memory>
#include <unordered_map>

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vulkan.hpp>

#include "instance_context.hh"

namespace low_latency {

class QueueContext;

struct DeviceContext {
    InstanceContext& instance;

    const VkDevice device;
    const VkuDeviceDispatchTable vtable;

    std::unordered_map<VkQueue, std::unique_ptr<QueueContext>> queue_contexts;

  public:
    DeviceContext(InstanceContext& parent_instance, const VkDevice& device,
                  VkuDeviceDispatchTable&& vtable);
    DeviceContext(const DeviceContext&) = delete;
    DeviceContext(DeviceContext&&) = delete;
    DeviceContext operator==(const DeviceContext&) = delete;
    DeviceContext operator==(DeviceContext&&) = delete;
};

}; // namespace low_latency

#endif
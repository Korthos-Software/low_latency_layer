#ifndef DEVICE_CONTEXT_HH_
#define DEVICE_CONTEXT_HH_

#include <memory>
#include <unordered_map>

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.hpp>

#include "context.hh"
#include "instance_context.hh"

namespace low_latency {

class QueueContext;

struct DeviceContext final : public Context {
    InstanceContext& instance;

    const VkDevice device;
    const VkuDeviceDispatchTable vtable;

    // Do we need to use this unless we wrap dispatchable objects?
    const PFN_vkSetDeviceLoaderData sdld;

    std::unordered_map<VkQueue, std::shared_ptr<QueueContext>> queues;

  public:
    DeviceContext(InstanceContext& parent_instance, const VkDevice& device,
                  const PFN_vkSetDeviceLoaderData& sdld,
                  VkuDeviceDispatchTable&& vtable);
    virtual ~DeviceContext();
};

}; // namespace low_latency

#endif
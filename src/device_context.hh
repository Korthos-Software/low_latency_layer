#ifndef DEVICE_CONTEXT_HH_
#define DEVICE_CONTEXT_HH_

#include <memory>
#include <shared_mutex>
#include <unordered_map>

#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include "context.hh"
#include "device_clock.hh"
#include "instance_context.hh"
#include "physical_device_context.hh"
#include "queue_context.hh"
#include "strategies/device_strategy.hh"

namespace low_latency {

class DeviceContext final : public Context {
  public:
    InstanceContext& instance;
    PhysicalDeviceContext& physical_device;
    // Whether or not we were asked to do NV_VK_LowLatency2 or VK_AMD_anti_lag
    // at the device level.
    const bool was_capability_requested;
    const VkDevice device;
    const VkuDeviceDispatchTable vtable;

    std::shared_mutex mutex;
    std::unique_ptr<DeviceClock> clock;
    std::unordered_map<VkQueue, std::shared_ptr<QueueContext>> queues;
    std::unique_ptr<DeviceStrategy> strategy;

  public:
    DeviceContext(InstanceContext& parent_instance,
                  PhysicalDeviceContext& parent_physical,
                  const VkDevice& device, const bool was_capability_requested,
                  VkuDeviceDispatchTable&& vtable);
    virtual ~DeviceContext();
};

}; // namespace low_latency

#endif
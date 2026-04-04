#ifndef DEVICE_CONTEXT_HH_
#define DEVICE_CONTEXT_HH_

#include <chrono>
#include <memory>
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
#include "swapchain_monitor.hh"

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

    std::unique_ptr<DeviceClock> clock;

    std::unordered_map<VkQueue, std::shared_ptr<QueueContext>> queues;

    std::unordered_map<VkSwapchainKHR, std::unique_ptr<SwapchainMonitor>>
        swapchain_monitors;

  public:
    DeviceContext(InstanceContext& parent_instance,
                  PhysicalDeviceContext& parent_physical,
                  const VkDevice& device, const bool was_capability_requested,
                  VkuDeviceDispatchTable&& vtable);
    virtual ~DeviceContext();

  public:
    // Updates the settings associated with that swapchain. If no swapchain
    // target is provided all swapchains are set to this value.
    void update_params(const std::optional<VkSwapchainKHR> target,
                       const std::chrono::microseconds& present_delay,
                       const bool was_low_latency_requested);

    void notify_present(const VkSwapchainKHR& swapchain,
                        std::unique_ptr<QueueContext::Submissions> submissions);
};

}; // namespace low_latency

#endif
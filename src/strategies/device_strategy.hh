#ifndef STRATEGIES_DEVICE_STRATEGY_HH_
#define STRATEGIES_DEVICE_STRATEGY_HH_

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

namespace low_latency {

class DeviceContext;

class DeviceStrategy {
  protected:
    DeviceContext& device;

  public:
    DeviceStrategy(DeviceContext& device);
    virtual ~DeviceStrategy();

  public:
    virtual void
    notify_create_swapchain(const VkSwapchainKHR& swapchain,
                            const VkSwapchainCreateInfoKHR& info) = 0;
    virtual void notify_destroy_swapchain(const VkSwapchainKHR& swapchain) = 0;
};

} // namespace low_latency

#endif
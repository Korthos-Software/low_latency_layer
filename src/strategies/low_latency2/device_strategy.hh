#ifndef STRATEGIES_LOW_LATENCY2_DEVICE_STRATEGY_HH_
#define STRATEGIES_LOW_LATENCY2_DEVICE_STRATEGY_HH_

#include "strategies/device_strategy.hh"
#include "swapchain_monitor.hh"

#include <shared_mutex>
#include <unordered_map>

namespace low_latency {

class DeviceContext;

class LowLatency2DeviceStrategy final : public DeviceStrategy {
  private:
    std::shared_mutex mutex;
    // swapchain -> swapchain monitor
    std::unordered_map<VkSwapchainKHR, SwapchainMonitor> swapchain_monitors;

  public:
    LowLatency2DeviceStrategy(DeviceContext& device);
    virtual ~LowLatency2DeviceStrategy();

  public:
    virtual void
    notify_create_swapchain(const VkSwapchainKHR& swapchain,
                            const VkSwapchainCreateInfoKHR& info) override;
    virtual void
    notify_destroy_swapchain(const VkSwapchainKHR& swapchain) override;
};

} // namespace low_latency

#endif

#ifndef STRATEGIES_ANTI_LAG_DEVICE_STRATEGY_HH_
#define STRATEGIES_ANTI_LAG_DEVICE_STRATEGY_HH_

#include "atomic_time_point.hh"
#include "strategies/device_strategy.hh"

#include <vulkan/vulkan.h>

#include <chrono>
#include <optional>
#include <shared_mutex>

namespace low_latency {

class DeviceContext;

class AntiLagDeviceStrategy final : public DeviceStrategy {
  private:
    std::shared_mutex mutex{};
    // If this is nullopt don't track the submission.
    std::optional<std::uint64_t> frame_index{};
    std::chrono::microseconds input_delay{};
    bool is_enabled{};

    AtomicTimePoint previous_input_release;

  public:
    AntiLagDeviceStrategy(DeviceContext& device);
    virtual ~AntiLagDeviceStrategy();

  public:
    virtual void
    notify_create_swapchain(const VkSwapchainKHR& swapchain,
                            const VkSwapchainCreateInfoKHR& info) override;
    virtual void
    notify_destroy_swapchain(const VkSwapchainKHR& swapchain) override;

  public:
    void notify_update(const VkAntiLagDataAMD& data);

    bool should_track_submissions();
};

} // namespace low_latency

#endif

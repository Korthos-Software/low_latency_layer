#ifndef STRATEGIES_ANTI_LAG_DEVICE_STRATEGY_HH_
#define STRATEGIES_ANTI_LAG_DEVICE_STRATEGY_HH_

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
    std::optional<std::chrono::steady_clock::time_point> previous_input_release;
    std::chrono::microseconds delay{};
    bool is_enabled{};

  public:
    AntiLagDeviceStrategy(DeviceContext& device);
    virtual ~AntiLagDeviceStrategy();

  public:
    void notify_update(const VkAntiLagDataAMD& data);

    bool should_track_submissions();
};

} // namespace low_latency

#endif

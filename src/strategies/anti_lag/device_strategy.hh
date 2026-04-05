#ifndef STRATEGIES_ANTI_LAG_DEVICE_STRATEGY_HH_
#define STRATEGIES_ANTI_LAG_DEVICE_STRATEGY_HH_

#include "strategies/device_strategy.hh"

namespace low_latency {

class DeviceContext;

class AntiLagDeviceStrategy final : public DeviceStrategy {
  public:
    AntiLagDeviceStrategy(DeviceContext& device);
    virtual ~AntiLagDeviceStrategy();
};

} // namespace low_latency

#endif

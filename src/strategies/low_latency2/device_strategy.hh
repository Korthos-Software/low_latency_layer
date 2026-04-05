#ifndef STRATEGIES_LOW_LATENCY2_DEVICE_STRATEGY_HH_
#define STRATEGIES_LOW_LATENCY2_DEVICE_STRATEGY_HH_

#include "strategies/device_strategy.hh"

namespace low_latency {

class DeviceContext;

class LowLatency2DeviceStrategy final : public DeviceStrategy {
  public:
    LowLatency2DeviceStrategy(DeviceContext& device);
    virtual ~LowLatency2DeviceStrategy();
};

} // namespace low_latency

#endif

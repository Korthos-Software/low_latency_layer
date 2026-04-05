#ifndef STRATEGIES_DEVICE_STRATEGY_HH_
#define STRATEGIES_DEVICE_STRATEGY_HH_

namespace low_latency {

class DeviceContext;

class DeviceStrategy {
    DeviceContext& device;

  public:
    DeviceStrategy(DeviceContext& device);
    virtual ~DeviceStrategy();
};

} // namespace low_latency

#endif
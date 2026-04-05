#ifndef STRATEGIES_DEVICE_STRATEGY_HH_
#define STRATEGIES_DEVICE_STRATEGY_HH_

namespace low_latency {

class DeviceContext;

class DeviceStrategy {
  protected:
    DeviceContext& device;

  public:
    DeviceStrategy(DeviceContext& device);
    virtual ~DeviceStrategy();

  public:

};

} // namespace low_latency

#endif
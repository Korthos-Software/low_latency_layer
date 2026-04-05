#include "device_strategy.hh"

namespace low_latency {

AntiLagDeviceStrategy::AntiLagDeviceStrategy(DeviceContext& device)
    : DeviceStrategy(device) {}

AntiLagDeviceStrategy::~AntiLagDeviceStrategy() {}

} // namespace low_latency
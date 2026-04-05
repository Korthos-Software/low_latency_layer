#include "device_strategy.hh"

namespace low_latency {

DeviceStrategy::DeviceStrategy(DeviceContext& device) : device(device) {}

DeviceStrategy::~DeviceStrategy() {}

} // namespace low_latency
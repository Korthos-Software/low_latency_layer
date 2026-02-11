#include "physical_device_context.hh"

namespace low_latency {

PhysicalDeviceContext::PhysicalDeviceContext(
    InstanceContext& instance_context, const VkPhysicalDevice& physical_device)
    : instance(instance_context), physical_device(physical_device) {}

PhysicalDeviceContext::~PhysicalDeviceContext() {}

} // namespace low_latency
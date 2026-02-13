#include "physical_device_context.hh"

namespace low_latency {
    
PhysicalDeviceContext::PhysicalDeviceContext(
    InstanceContext& instance_context, const VkPhysicalDevice& physical_device)
    : instance(instance_context), physical_device(physical_device) {

    auto props = VkPhysicalDeviceProperties{};
    instance.vtable.GetPhysicalDeviceProperties(this->physical_device, &props);
    this->properties =
        std::make_unique<VkPhysicalDeviceProperties>(std::move(props));
}

PhysicalDeviceContext::~PhysicalDeviceContext() {}

} // namespace low_latency
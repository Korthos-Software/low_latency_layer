#ifndef PHYSICAL_DEVICE_CONTEXT_HH_
#define PHYSICAL_DEVICE_CONTEXT_HH_

#include "instance_context.hh"

#include <vulkan/vulkan.hpp>

#include "context.hh"

namespace low_latency {

class PhysicalDeviceContext final : public Context {
  public:
    InstanceContext& instance;

    const VkPhysicalDevice physical_device;
    
    const std::unique_ptr<VkPhysicalDeviceProperties> properties;
    
    using queue_properties_t = std::vector<VkQueueFamilyProperties2>;
    const std::unique_ptr<queue_properties_t> queue_properties;

  public:
    PhysicalDeviceContext(InstanceContext& instance_context,
                          const VkPhysicalDevice& physical_device);
    virtual ~PhysicalDeviceContext();
};

} // namespace low_latency

#endif
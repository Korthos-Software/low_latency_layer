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

  public:
    PhysicalDeviceContext(InstanceContext& instance_context,
                          const VkPhysicalDevice& physical_device);
    virtual ~PhysicalDeviceContext();
};

} // namespace low_latency

#endif
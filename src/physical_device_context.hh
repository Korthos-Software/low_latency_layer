#ifndef PHYSICAL_DEVICE_CONTEXT_HH_
#define PHYSICAL_DEVICE_CONTEXT_HH_

#include "instance_context.hh"

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include "context.hh"

namespace low_latency {

class PhysicalDeviceContext final : public Context {
  public:
    // The extensions we need for our layer to function.
    // If the PD doesn't support this then the layer shouldn't set the anti lag
    // flag in VkGetPhysicalDevices2 (check this->supports_required_extensions).
    static constexpr auto required_extensions = {
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
        VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
        VK_KHR_PRESENT_ID_EXTENSION_NAME};

  public:
    InstanceContext& instance;
    const VkPhysicalDevice physical_device;

    std::unique_ptr<VkPhysicalDeviceProperties> properties;
    std::unique_ptr<std::vector<VkQueueFamilyProperties2>> queue_properties;

    // Will be true if the physical device supports all of required_extensions.
    bool supports_required_extensions = false;

  public:
    PhysicalDeviceContext(InstanceContext& instance_context,
                          const VkPhysicalDevice& physical_device);
    virtual ~PhysicalDeviceContext();
};

} // namespace low_latency

#endif
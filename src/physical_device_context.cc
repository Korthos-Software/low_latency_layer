#include "physical_device_context.hh"
#include <vulkan/vulkan_core.h>

namespace low_latency {

PhysicalDeviceContext::PhysicalDeviceContext(
    InstanceContext& instance_context, const VkPhysicalDevice& physical_device)
    : instance(instance_context), physical_device(physical_device) {

    const auto& vtable = instance_context.vtable;

    this->properties = [&]() {
        auto props = VkPhysicalDeviceProperties{};
        vtable.GetPhysicalDeviceProperties(physical_device, &props);
        return std::make_unique<VkPhysicalDeviceProperties>(std::move(props));
    }();

    this->queue_properties = [&]() {
        auto count = std::uint32_t{};
        vtable.GetPhysicalDeviceQueueFamilyProperties2(physical_device, &count,
                                                       nullptr);

        using qp_t = PhysicalDeviceContext::queue_properties_t;
        auto result = qp_t(
            count, VkQueueFamilyProperties2{
                       .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
        vtable.GetPhysicalDeviceQueueFamilyProperties2(physical_device, &count,
                                                       std::data(result));

        return std::make_unique<qp_t>(std::move(result));
    }();
}

PhysicalDeviceContext::~PhysicalDeviceContext() {}

} // namespace low_latency
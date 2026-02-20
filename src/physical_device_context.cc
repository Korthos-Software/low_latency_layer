#include "physical_device_context.hh"
#include <vulkan/vulkan_core.h>

namespace low_latency {

static std::unique_ptr<VkPhysicalDeviceProperties>
make_pd_props(const InstanceContext& instance_context,
              const VkPhysicalDevice& physical_device) {
    const auto& vtable = instance_context.vtable;

    auto props = VkPhysicalDeviceProperties{};
    vtable.GetPhysicalDeviceProperties(physical_device, &props);
    return std::make_unique<VkPhysicalDeviceProperties>(std::move(props));
}

static std::unique_ptr<PhysicalDeviceContext::queue_properties_t>
make_qf_props(const InstanceContext& instance_context,
              const VkPhysicalDevice& physical_device) {

    const auto& vtable = instance_context.vtable;

    auto count = std::uint32_t{};
    vtable.GetPhysicalDeviceQueueFamilyProperties2(physical_device, &count,
                                                   nullptr);

    auto result = std::vector<VkQueueFamilyProperties2>(
        count, VkQueueFamilyProperties2{
                   .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
    vtable.GetPhysicalDeviceQueueFamilyProperties2(physical_device, &count,
                                                   std::data(result));

    using qp_t = PhysicalDeviceContext::queue_properties_t;
    return std::make_unique<qp_t>(std::move(result));
}

PhysicalDeviceContext::PhysicalDeviceContext(
    InstanceContext& instance_context, const VkPhysicalDevice& physical_device)
    : instance(instance_context), physical_device(physical_device),
      properties(make_pd_props(instance, physical_device)),
      queue_properties(make_qf_props(instance, physical_device)) {}

PhysicalDeviceContext::~PhysicalDeviceContext() {}

} // namespace low_latency
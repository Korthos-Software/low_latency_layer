#include "physical_device_context.hh"

#include <vulkan/vulkan_core.h>

#include <ranges>
#include <string_view>
#include <unordered_set>
#include <vector>

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

    this->supports_required_extensions = [&]() {
        auto count = std::uint32_t{};
        vtable.EnumerateDeviceExtensionProperties(physical_device, nullptr,
                                                  &count, nullptr);

        auto supported_extensions = std::vector<VkExtensionProperties>(count);
        vtable.EnumerateDeviceExtensionProperties(
            physical_device, nullptr, &count, std::data(supported_extensions));

        const auto supported =
            supported_extensions |
            std::views::transform(
                [](const auto& supported) { return supported.extensionName; }) |
            std::ranges::to<std::unordered_set<std::string_view>>();

        return std::ranges::all_of(
            this->required_extensions, [&](const auto& required_extension) {
                return supported.contains(required_extension);
            });
    }();
}

PhysicalDeviceContext::~PhysicalDeviceContext() {}

} // namespace low_latency
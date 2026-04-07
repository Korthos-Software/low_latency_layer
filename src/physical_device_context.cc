#include "physical_device_context.hh"
#include "helper.hh"

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

    // Check if we support Vulkan 1.1 by checking if this function exists. If we
    // don't, the layer cannot work, so we set 'supports required extensions' to
    // false and bail.
    if (!vtable.GetPhysicalDeviceQueueFamilyProperties2KHR) {
        this->supports_required_extensions = false;
        return;
    }

    this->queue_properties = [&]() {
        auto count = std::uint32_t{};
        vtable.GetPhysicalDeviceQueueFamilyProperties2KHR(physical_device,
                                                          &count, nullptr);

        auto result = std::vector<VkQueueFamilyProperties2>(
            count, VkQueueFamilyProperties2{
                       .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
        vtable.GetPhysicalDeviceQueueFamilyProperties2KHR(
            physical_device, &count, std::data(result));
        return std::make_unique<std::vector<VkQueueFamilyProperties2>>(
            std::move(result));
    }();

    this->supports_required_extensions = [&]() {
        auto count = std::uint32_t{};
        THROW_NOT_VKSUCCESS(vtable.EnumerateDeviceExtensionProperties(
            physical_device, nullptr, &count, nullptr));

        auto supported_extensions = std::vector<VkExtensionProperties>(count);
        THROW_NOT_VKSUCCESS(vtable.EnumerateDeviceExtensionProperties(
            physical_device, nullptr, &count, std::data(supported_extensions)));

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
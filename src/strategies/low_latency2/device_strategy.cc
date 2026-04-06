#include "device_strategy.hh"

#include "helper.hh"
#include <mutex>

namespace low_latency {

LowLatency2DeviceStrategy::LowLatency2DeviceStrategy(DeviceContext& device)
    : DeviceStrategy(device) {}

LowLatency2DeviceStrategy::~LowLatency2DeviceStrategy() {}

void LowLatency2DeviceStrategy::notify_create_swapchain(
    const VkSwapchainKHR& swapchain, const VkSwapchainCreateInfoKHR& info) {

    // VK_NV_low_latency2 allows a swapchain to be created with the low latency
    // mode already on via VkSwapchainLatencyCreateInfoNV.
    auto was_low_latency_requested = bool{false};
    if (const auto slci = find_next<VkSwapchainLatencyCreateInfoNV>(
            &info, VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV);
        slci) {

        was_low_latency_requested = slci->latencyModeEnable;
    }

    const auto lock = std::scoped_lock{this->mutex};
    const auto iter = this->swapchain_monitors.emplace(swapchain, this->device);
    assert(iter.second);
    iter.first->second.update_params(was_low_latency_requested,
                                     std::chrono::microseconds{0});
}

void LowLatency2DeviceStrategy::notify_destroy_swapchain(
    const VkSwapchainKHR& swapchain) {

    const auto lock = std::scoped_lock{this->mutex};

    this->swapchain_monitors.erase(swapchain);
}

} // namespace low_latency

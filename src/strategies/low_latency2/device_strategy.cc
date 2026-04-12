#include "device_strategy.hh"
#include "device_context.hh"
#include "queue_strategy.hh"

#include "helper.hh"
#include <mutex>
#include <vulkan/vulkan_core.h>

namespace low_latency {

LowLatency2DeviceStrategy::LowLatency2DeviceStrategy(DeviceContext& device)
    : DeviceStrategy(device) {}

LowLatency2DeviceStrategy::~LowLatency2DeviceStrategy() {}

void LowLatency2DeviceStrategy::notify_create_swapchain(
    const VkSwapchainKHR& swapchain, const VkSwapchainCreateInfoKHR& info) {

    // VK_NV_low_latency2 allows a swapchain to be created with the low latency
    // mode already on via VkSwapchainLatencyCreateInfoNV.
    // Default to enabled - if the app is using VK_NV_low_latency2 at all it
    // wants pacing. VkSwapchainLatencyCreateInfoNV can override this, but
    // apps like CS2 recreate swapchains without it (apparent app bug).
    auto was_low_latency_requested = bool{true};
    if (const auto slci = find_next<VkSwapchainLatencyCreateInfoNV>(
            &info, VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV);
        slci) {

        was_low_latency_requested = slci->latencyModeEnable;
    }

    const auto lock = std::scoped_lock{this->mutex};
    const auto iter = this->swapchain_monitors.emplace(swapchain, this->device);
    iter.first->second.update_params(was_low_latency_requested,
                                     std::chrono::microseconds{0});
}

void LowLatency2DeviceStrategy::notify_destroy_swapchain(
    const VkSwapchainKHR& swapchain) {

    const auto lock = std::scoped_lock{this->mutex};

    this->swapchain_monitors.erase(swapchain);
}

void LowLatency2DeviceStrategy::notify_latency_sleep_mode(
    const VkSwapchainKHR& swapchain,
    const VkLatencySleepModeInfoNV* const info) {

    const auto lock = std::shared_lock{this->mutex};

    const auto iter = this->swapchain_monitors.find(swapchain);
    if (iter == std::end(this->swapchain_monitors)) {
        return;
    }

    using namespace std::chrono;
    if (info) {
        iter->second.update_params(info->lowLatencyMode,
                                   microseconds{info->minimumIntervalUs});
    } else {
        iter->second.update_params(false, 0us);
    }
}

void LowLatency2DeviceStrategy::submit_swapchain_present_id(
    const VkSwapchainKHR& swapchain, const std::uint64_t& present_id) {

    // Iterate through all queues and grab any work that's associated with this
    // present_id. Turn it into a vector of work that we give to our swapchain
    // monitor.
    auto work = [&]() -> std::vector<std::unique_ptr<FrameSpan>> {
        auto work = std::vector<std::unique_ptr<FrameSpan>>{};
        const auto lock = std::shared_lock{this->device.mutex};
        for (const auto& queue_iter : this->device.queues) {
            const auto& queue = queue_iter.second;

            const auto strategy =
                dynamic_cast<LowLatency2QueueStrategy*>(queue->strategy.get());
            assert(strategy);

            if (strategy->is_out_of_band.load(std::memory_order::relaxed)) {
                continue;
            }

            // Need the lock now - we're modifying it.
            const auto strategy_lock = std::unique_lock{strategy->mutex};
            const auto iter = strategy->frame_spans.find(present_id);
            if (iter == std::end(strategy->frame_spans)) {
                continue;
            }

            // Make sure we clean it up from the present as well.
            work.push_back(std::move(iter->second));
            strategy->frame_spans.erase(iter);
        }
        return work;
    }();

    const auto lock = std::scoped_lock{this->mutex};
    const auto iter = this->swapchain_monitors.find(swapchain);
    if (iter == std::end(this->swapchain_monitors)) {
        return;
    }
    // Notify our monitor that this work has to be completed before they signal
    // whatever semaphore is currently sitting in it.
    iter->second.attach_work(std::move(work));
}

void LowLatency2DeviceStrategy::notify_latency_sleep_nv(
    const VkSwapchainKHR& swapchain, const VkLatencySleepInfoNV& info) {

    const auto lock = std::scoped_lock{this->mutex};

    const auto semaphore_signal =
        SemaphoreSignal{info.signalSemaphore, info.value};

    const auto iter = this->swapchain_monitors.find(swapchain);
    if (iter == std::end(this->swapchain_monitors)) {
        // If we can't find the swapchain we have to signal the semaphore
        // anyway. We must *never* discard these semaphores without signalling
        // them first.
        semaphore_signal.signal(this->device);
        return;
    }
    iter->second.notify_semaphore(semaphore_signal);
}

} // namespace low_latency

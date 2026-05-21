#include "device_strategy.hh"
#include "device_context.hh"

#include "queue_strategy.hh"

#include <vulkan/vulkan_core.h>

namespace low_latency {

AntiLagDeviceStrategy::AntiLagDeviceStrategy(DeviceContext& device)
    : DeviceStrategy(device),
      delay_controller(device.instance.is_simulation_decoupled,
                       device.instance.should_strict_sync) {}

AntiLagDeviceStrategy::~AntiLagDeviceStrategy() {}

void AntiLagDeviceStrategy::notify_update(const VkAntiLagDataAMD& data) {
    auto lock = std::unique_lock{this->mutex};

    this->is_enabled = data.mode != VK_ANTI_LAG_MODE_OFF_AMD;
    const auto min_delay = [&]() -> std::chrono::microseconds {
        using namespace std::chrono;
        if (!data.maxFPS) {
            return 0us;
        }
        return duration_cast<microseconds>(1s) / data.maxFPS;
    }();

    if (!data.pPresentationInfo || !is_enabled) {
        return;
    }

    // If we're at the present stage, stop collecting submissions by making
    // our frame_index nullopt.
    if (data.pPresentationInfo->stage == VK_ANTI_LAG_STAGE_PRESENT_AMD) {
        this->frame_index.reset();
        return;
    }
    // If we're at the input stage, start marking submissions as relevant.
    this->frame_index.emplace(data.pPresentationInfo->frameIndex);

    lock.unlock();

    // We need to collect all queue submission and wait on them in this thread.
    // Input stage needs to wait for all queue submissions to complete.
    const auto work = [&]() -> auto {
        auto work = std::vector<std::unique_ptr<SubmissionSpan>>{};
        const auto device_lock = std::shared_lock{this->device.mutex};
        for (const auto& iter : this->device.queues) {
            const auto& queue = iter.second;

            const auto strategy =
                dynamic_cast<AntiLagQueueStrategy*>(queue->strategy.get());
            assert(strategy);

            // Grab it from the queue, don't hold the lock.
            const auto queue_lock = std::scoped_lock{strategy->mutex};
            work.emplace_back(std::move(strategy->submission_span));
            strategy->submission_span.reset();
        }
        return work;
    }();

    this->delay_controller.delay(min_delay, work);
}

bool AntiLagDeviceStrategy::should_track_submissions() {
    const auto lock = std::shared_lock{this->mutex};

    if (!this->is_enabled) {
        return false;
    }

    // Don't track submissions if our frame index is nullopt!
    if (!this->frame_index.has_value()) {
        return false;
    }

    return true;
}

// Stub - anti_lag doesn't differentiate between swapchains.
void AntiLagDeviceStrategy::notify_create_swapchain(
    const VkSwapchainKHR&, const VkSwapchainCreateInfoKHR&) {}

// Stub - again, AL doesn't care about swapchains.
void AntiLagDeviceStrategy::notify_destroy_swapchain(const VkSwapchainKHR&) {}

} // namespace low_latency
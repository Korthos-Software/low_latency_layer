#include "device_strategy.hh"
#include "device_context.hh"

#include "queue_strategy.hh"

#include <vulkan/vulkan_core.h>

namespace low_latency {

AntiLagDeviceStrategy::AntiLagDeviceStrategy(DeviceContext& device)
    : DeviceStrategy(device) {}

AntiLagDeviceStrategy::~AntiLagDeviceStrategy() {}

void AntiLagDeviceStrategy::notify_update(const VkAntiLagDataAMD& data) {
    const auto lock = std::scoped_lock{this->mutex};

    this->is_enabled = !(data.mode == VK_ANTI_LAG_MODE_OFF_AMD);

    this->delay = [&]() -> std::chrono::microseconds {
        using namespace std::chrono;
        if (!data.maxFPS) {
            return 0us;
        }
        return duration_cast<microseconds>(1s / data.maxFPS);
    }();

    if (!data.pPresentationInfo) {
        return;
    }

    // If we're at the input stage, start marking submissions as relevant.
    // If we're at the present stage, stop collecting submissions by making
    // our frame_index nullopt.
    if (data.pPresentationInfo->stage == VK_ANTI_LAG_STAGE_PRESENT_AMD) {
        this->frame_index.reset();
        return;
    }
    this->frame_index.emplace(data.pPresentationInfo->frameIndex);

    // We're in input now. Wait for all queue submissions to complete.
    const auto device_lock = std::shared_lock{this->device.mutex};
    for (const auto& iter : this->device.queues) {
        const auto& queue = iter.second;

        const auto strategy =
            dynamic_cast<AntiLagQueueStrategy*>(queue->strategy.get());
        assert(strategy);

        strategy->await_complete();
    }
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

} // namespace low_latency
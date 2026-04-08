#include "queue_strategy.hh"
#include "device_context.hh"
#include "device_strategy.hh"
#include "helper.hh"
#include "queue_context.hh"

#include <vulkan/vulkan_core.h>

namespace low_latency {

LowLatency2QueueStrategy::LowLatency2QueueStrategy(QueueContext& queue)
    : QueueStrategy(queue) {}

LowLatency2QueueStrategy::~LowLatency2QueueStrategy() {}

template <typename T>
static void notify_submit_impl(LowLatency2QueueStrategy& strategy,
                               const T& submit,
                               std::shared_ptr<TimestampPool::Handle> handle) {

    // It's actually not a requirement that we have this present id.
    const auto present_id = [&]() -> std::uint64_t {
        const auto lspi = find_next<VkLatencySubmissionPresentIdNV>(
            &submit, VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV);
        return lspi ? lspi->presentID : 0;
    }();

    const auto lock = std::scoped_lock{strategy.mutex};
    const auto [iter, inserted] = strategy.frame_spans.try_emplace(present_id);
    if (inserted) {
        iter->second = std::make_unique<FrameSpan>(std::move(handle));
        // Add our present_id to our ring tracking if it's non-zero.
        if (present_id) {
            strategy.stale_present_ids.push_back(present_id);
        }
    } else {
        iter->second->update(std::move(handle));
    }

    // Remove stale present_id's if they weren't presented to.
    if (std::size(strategy.stale_present_ids) >
        LowLatency2QueueStrategy::MAX_TRACKED_PRESENTS) {

        const auto stale_present_id = strategy.stale_present_ids.front();
        strategy.stale_present_ids.pop_front();
        strategy.frame_spans.erase(stale_present_id);
    }
}

void LowLatency2QueueStrategy::notify_submit(
    const VkSubmitInfo& submit,
    std::shared_ptr<TimestampPool::Handle> submission) {

    notify_submit_impl(*this, submit, std::move(submission));
}

void LowLatency2QueueStrategy::notify_submit(
    const VkSubmitInfo2& submit,
    std::shared_ptr<TimestampPool::Handle> submission) {

    notify_submit_impl(*this, submit, std::move(submission));
}

void LowLatency2QueueStrategy::notify_present(const VkPresentInfoKHR& present) {

    const auto pid =
        find_next<VkPresentIdKHR>(&present, VK_STRUCTURE_TYPE_PRESENT_ID_KHR);

    const auto device_strategy = dynamic_cast<LowLatency2DeviceStrategy*>(
        this->queue.device.strategy.get());
    assert(device_strategy);

    for (auto i = std::uint32_t{0}; i < present.swapchainCount; ++i) {
        const auto& swapchain = present.pSwapchains[i];
        const auto present_id = [&]() -> std::uint64_t {
            if (pid && pid->pPresentIds) {
                return pid->pPresentIds[i];
            }
            return 0;
        }();
        device_strategy->submit_swapchain_present_id(swapchain, present_id);
    }
}

void LowLatency2QueueStrategy::notify_out_of_band() {
    this->is_out_of_band.store(true, std::memory_order_relaxed);
}

} // namespace low_latency

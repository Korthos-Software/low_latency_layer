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
                               std::unique_ptr<Submission> submission) {

    // It's actually not a requirement that we have this present id.
    const auto lspi = find_next<VkLatencySubmissionPresentIdNV>(
        &submit, VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV);
    const auto present_id = lspi ? lspi->presentID : 0;

    const auto lock = std::scoped_lock{strategy.mutex};
    const auto [iter, inserted] =
        strategy.present_id_submissions.try_emplace(present_id);
    iter->second.push_back(std::move(submission));

    // Remove stale submissions if we're presenting a lot to the same
    // present_id. This doesn't affect anything because we're waiting on the
    // last. It begs the question: should we should just store the last only?
    if (std::size(iter->second) >=
        LowLatency2QueueStrategy::MAX_TRACKED_OBJECTS) {

        iter->second.pop_front();
    }

    // Add our present_id to our ring tracking if it's non-zero.
    if (inserted && present_id) {
        strategy.present_id_ring.push_back(present_id);
    }

    // Remove stale present_id's if they weren't presented to.
    if (std::size(strategy.present_id_ring) >
        LowLatency2QueueStrategy::MAX_TRACKED_OBJECTS) {

        const auto to_remove = strategy.present_id_ring.front();
        strategy.present_id_ring.pop_front();
        strategy.present_id_submissions.erase(to_remove);
    }
}

void LowLatency2QueueStrategy::notify_submit(
    const VkSubmitInfo& submit, std::unique_ptr<Submission> submission) {

    notify_submit_impl(*this, submit, std::move(submission));
}

void LowLatency2QueueStrategy::notify_submit(
    const VkSubmitInfo2& submit, std::unique_ptr<Submission> submission) {

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

#include "queue_strategy.hh"
#include "helper.hh"

#include <ranges>
#include <span>

namespace low_latency {

LowLatency2QueueStrategy::LowLatency2QueueStrategy(QueueContext& queue)
    : QueueStrategy(queue) {}

LowLatency2QueueStrategy::~LowLatency2QueueStrategy() {}

void LowLatency2QueueStrategy::notify_submit(
    [[maybe_unused]] const VkSubmitInfo& submit,
    [[maybe_unused]] std::unique_ptr<Submission> submission) {}

void LowLatency2QueueStrategy::notify_submit(
    [[maybe_unused]] const VkSubmitInfo2& submit,
    [[maybe_unused]] std::unique_ptr<Submission> submission) {}

void LowLatency2QueueStrategy::notify_present(const VkPresentInfoKHR& present) {

    const auto pid =
        find_next<VkPresentIdKHR>(&present, VK_STRUCTURE_TYPE_PRESENT_ID_KHR);

    // All submissions should be tagged with a present_id. If it isn't, I'm not
    // going to fail hard here - we will just ignore it.
    if (!pid) {
        return;
    }

    const auto swapchains =
        std::span{present.pSwapchains, present.swapchainCount};
    const auto present_ids =
        std::span{pid->pPresentIds, present.swapchainCount};
    for (const auto& [swapchain, present_id] :
         std::views::zip(swapchains, present_ids)) {

        // TODO
    }
}

} // namespace low_latency

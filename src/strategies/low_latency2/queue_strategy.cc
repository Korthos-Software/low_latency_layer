#include "queue_strategy.hh"

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

} // namespace low_latency

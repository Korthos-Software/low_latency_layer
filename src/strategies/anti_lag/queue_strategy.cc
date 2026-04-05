#include "queue_strategy.hh"
#include "device_context.hh"
#include "device_strategy.hh"
#include "queue_context.hh"

namespace low_latency {

AntiLagQueueStrategy::AntiLagQueueStrategy(QueueContext& queue)
    : QueueStrategy(queue) {}

AntiLagQueueStrategy::~AntiLagQueueStrategy() {}

void AntiLagQueueStrategy::notify_submit(
    [[maybe_unused]] const VkSubmitInfo& submit,
    std::unique_ptr<Submission> submission) {

    const auto strategy =
        dynamic_cast<AntiLagDeviceStrategy*>(this->queue.device.strategy.get());
    assert(strategy);
    if (!strategy->should_track_submissions()) {
        return;
    }

    const auto lock = std::scoped_lock(this->mutex);
    this->pending_submissions.push_back(std::move(submission));
}

void AntiLagQueueStrategy::notify_submit(
    [[maybe_unused]] const VkSubmitInfo2& submit,
    std::unique_ptr<Submission> submission) {

    const auto strategy =
        dynamic_cast<AntiLagDeviceStrategy*>(this->queue.device.strategy.get());
    assert(strategy);
    if (!strategy->should_track_submissions()) {
        return;
    }

    const auto lock = std::scoped_lock(this->mutex);
    this->pending_submissions.push_back(std::move(submission));
}

void AntiLagQueueStrategy::await_complete() {

    // Grab submissions while under a lock.
    const auto submissions = [&]() -> std::deque<std::unique_ptr<Submission>> {
        const auto lock = std::scoped_lock{this->mutex};

        auto submissions = std::move(this->pending_submissions);
        this->pending_submissions.clear();
        return submissions;
    }();

    // Wait for completion on the last submission.
    if (submissions.empty()) {
        return;
    }
    const auto& last = submissions.back();
    last->end->await_time();
}

} // namespace low_latency

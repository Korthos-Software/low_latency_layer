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
    std::shared_ptr<TimestampPool::Handle> handle) {

    const auto strategy =
        dynamic_cast<AntiLagDeviceStrategy*>(this->queue.device.strategy.get());
    assert(strategy);
    if (!strategy->should_track_submissions()) {
        return;
    }

    const auto lock = std::scoped_lock(this->mutex);
    if (this->frame_span) {
        this->frame_span->update(std::move(handle));
    } else {
        this->frame_span = std::make_unique<FrameSpan>(std::move(handle));
    }
}

void AntiLagQueueStrategy::notify_submit(
    [[maybe_unused]] const VkSubmitInfo2& submit,
    std::shared_ptr<TimestampPool::Handle> handle) {

    const auto strategy =
        dynamic_cast<AntiLagDeviceStrategy*>(this->queue.device.strategy.get());
    assert(strategy);
    if (!strategy->should_track_submissions()) {
        return;
    }

    const auto lock = std::scoped_lock(this->mutex);
    if (this->frame_span) {
        this->frame_span->update(std::move(handle));
    } else {
        this->frame_span = std::make_unique<FrameSpan>(std::move(handle));
    }
}

// Stub - AntiLag doesn't care about presents.
void AntiLagQueueStrategy::notify_present(const VkPresentInfoKHR&) {}

} // namespace low_latency

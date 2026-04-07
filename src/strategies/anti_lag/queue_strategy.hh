#ifndef STRATEGIES_ANTI_LAG_QUEUE_STRATEGY_HH_
#define STRATEGIES_ANTI_LAG_QUEUE_STRATEGY_HH_

#include "strategies/queue_strategy.hh"

#include "frame_span.hh"
#include <memory>
#include <mutex>

namespace low_latency {

class QueueContext;

class AntiLagQueueStrategy final : public QueueStrategy {
  public:
    std::mutex mutex;
    std::unique_ptr<FrameSpan> frame_span; // Null represents no work.

  public:
    AntiLagQueueStrategy(QueueContext& queue);
    virtual ~AntiLagQueueStrategy();

  public:
    virtual void
    notify_submit(const VkSubmitInfo& submit,
                  std::shared_ptr<TimestampPool::Handle> handle) override;
    virtual void
    notify_submit(const VkSubmitInfo2& submit,
                  std::shared_ptr<TimestampPool::Handle> handle) override;
    virtual void notify_present(const VkPresentInfoKHR& present) override;
};

} // namespace low_latency

#endif

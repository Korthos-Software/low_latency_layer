#ifndef STRATEGIES_LOW_LATENCY2_QUEUE_STRATEGY_HH_
#define STRATEGIES_LOW_LATENCY2_QUEUE_STRATEGY_HH_

#include "strategies/queue_strategy.hh"

namespace low_latency {

class QueueContext;

class LowLatency2QueueStrategy final : public QueueStrategy {
  public:
    LowLatency2QueueStrategy(QueueContext& queue);
    virtual ~LowLatency2QueueStrategy();

  public:
    virtual void notify_submit(const VkSubmitInfo& submit,
                               std::unique_ptr<Submission> submission) override;
    virtual void notify_submit(const VkSubmitInfo2& submit,
                               std::unique_ptr<Submission> submission) override;
    virtual void notify_present(const VkPresentInfoKHR& present) override;
};

} // namespace low_latency

#endif

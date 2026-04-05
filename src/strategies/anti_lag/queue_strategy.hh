#ifndef STRATEGIES_ANTI_LAG_QUEUE_STRATEGY_HH_
#define STRATEGIES_ANTI_LAG_QUEUE_STRATEGY_HH_

#include "strategies/queue_strategy.hh"

#include <deque>
#include <memory>
#include <mutex>

namespace low_latency {

class QueueContext;

class AntiLagQueueStrategy final : public QueueStrategy {
  private:
    std::mutex mutex;
    std::deque<std::unique_ptr<Submission>> pending_submissions;

  public:
    AntiLagQueueStrategy(QueueContext& queue);
    virtual ~AntiLagQueueStrategy();

  public:
    virtual void notify_submit(const VkSubmitInfo& submit,
                               std::unique_ptr<Submission> submission) override;
    virtual void notify_submit(const VkSubmitInfo2& submit,
                               std::unique_ptr<Submission> submission) override;

  public:
    // Wait for all pending submissions to complete. Resets pending submissions
    // once done.
    void await_complete();
};

} // namespace low_latency

#endif

#ifndef STRATEGIES_QUEUE_STRATEGY_HH_
#define STRATEGIES_QUEUE_STRATEGY_HH_

#include "timestamp_pool.hh"

#include <vulkan/vulkan.h>

namespace low_latency {

class QueueContext;

class QueueStrategy {
  protected:
    QueueContext& queue;

  public:
    QueueStrategy(QueueContext& queue);
    virtual ~QueueStrategy();

  public:
    virtual void
    notify_submit(const VkSubmitInfo& submit,
                  std::shared_ptr<TimestampPool::Handle> handle) = 0;
    virtual void
    notify_submit(const VkSubmitInfo2& submit,
                  std::shared_ptr<TimestampPool::Handle> handle) = 0;
    virtual void notify_present(const VkPresentInfoKHR& present) = 0;
};

} // namespace low_latency

#endif
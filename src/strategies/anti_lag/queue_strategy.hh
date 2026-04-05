#ifndef STRATEGIES_ANTI_LAG_QUEUE_STRATEGY_HH_
#define STRATEGIES_ANTI_LAG_QUEUE_STRATEGY_HH_

#include "strategies/queue_strategy.hh"

namespace low_latency {

class QueueContext;

class AntiLagQueueStrategy final : public QueueStrategy {
  public:
    AntiLagQueueStrategy(QueueContext& queue);
    virtual ~AntiLagQueueStrategy();
};

} // namespace low_latency

#endif

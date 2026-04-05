#ifndef STRATEGIES_LOW_LATENCY2_QUEUE_STRATEGY_HH_
#define STRATEGIES_LOW_LATENCY2_QUEUE_STRATEGY_HH_

#include "strategies/queue_strategy.hh"

namespace low_latency {

class QueueContext;

class LowLatency2QueueStrategy final : public QueueStrategy {
  public:
    LowLatency2QueueStrategy(QueueContext& queue);
    virtual ~LowLatency2QueueStrategy();
};

} // namespace low_latency

#endif

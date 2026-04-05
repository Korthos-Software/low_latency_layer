#ifndef STRATEGIES_QUEUE_STRATEGY_HH_
#define STRATEGIES_QUEUE_STRATEGY_HH_

namespace low_latency {

class QueueContext;

class QueueStrategy {
    QueueContext& queue;

  public:
    QueueStrategy(QueueContext& queue);
    virtual ~QueueStrategy();
};

} // namespace low_latency

#endif
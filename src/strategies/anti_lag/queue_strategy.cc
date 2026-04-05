#include "queue_strategy.hh"

namespace low_latency {

AntiLagQueueStrategy::AntiLagQueueStrategy(QueueContext& queue)
    : QueueStrategy(queue) {}

AntiLagQueueStrategy::~AntiLagQueueStrategy() {}

} // namespace low_latency

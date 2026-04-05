#include "queue_strategy.hh"

#include "queue_context.hh"

namespace low_latency {

QueueStrategy::QueueStrategy(QueueContext& queue) : queue(queue) {}

QueueStrategy::~QueueStrategy() {}

} // namespace low_latency
#ifndef FRAME_SPAN_HH_ 
#define FRAME_SPAN_HH_

#include "timestamp_pool.hh"

namespace low_latency {

// A class which contains timestamps that represent a Queue's contribution to
// a frame. It reduces the (possibly) huge amount of TimestampPool::Handle's
// that a queue needs to keep track of. It only keeps at max two - the first
// head handle and the tail handle, which is allowed to be null in the case of
// only a single submission for that queue.
class FrameSpan {
  public:
    const std::shared_ptr<TimestampPool::Handle> head_handle;
    std::shared_ptr<TimestampPool::Handle> tail_handle;

  public:
    FrameSpan(std::shared_ptr<TimestampPool::Handle> handle);
    FrameSpan(const FrameSpan&) = delete;
    FrameSpan(FrameSpan&&) = delete;
    FrameSpan operator=(const FrameSpan&) = delete;
    FrameSpan operator=(FrameSpan&&) = delete;
    ~FrameSpan();

  public:
    // Update the framespan's tail to include this timestamp.
    void update(std::shared_ptr<TimestampPool::Handle> handle);

  public:
    // Wait for for GPU work to complete.
    void await_completed() const;
};

} // namespace low_latency

#endif
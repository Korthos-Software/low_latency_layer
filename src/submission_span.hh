#ifndef SUBMISSION_SPAN_HH_
#define SUBMISSION_SPAN_HH_

#include <utility>

#include "timestamp_pool.hh"

namespace low_latency {

// A class which contains timestamps that represent a Queue's contribution to
// a frame. It reduces the (possibly) huge amount of TimestampPool::Handle's
// that a queue needs to keep track of. It only keeps at max two - the first
// head handle and the tail handle, which is allowed to be null in the case of
// only a single submission for that queue.
class SubmissionSpan {
  public:
    const std::shared_ptr<TimestampPool::Handle> head_handle{};
    std::shared_ptr<TimestampPool::Handle> tail_handle{};

  public:
    explicit SubmissionSpan(std::shared_ptr<TimestampPool::Handle> handle);
    SubmissionSpan(const SubmissionSpan&) = delete;
    SubmissionSpan(SubmissionSpan&&) = delete;
    SubmissionSpan& operator=(const SubmissionSpan&) = delete;
    SubmissionSpan& operator=(SubmissionSpan&&) = delete;
    ~SubmissionSpan();

  public:
    // Update the tail to include this timestamp.
    void update(std::shared_ptr<TimestampPool::Handle> handle);

  public:
    // Check if GPU work has completed without blocking.
    bool has_completed() const;

    // Wait for GPU work to start - returns the start time.
    DeviceClock::time_point await_started() const;

    // Wait for GPU work to complete - returns the start and end time.
    std::pair<DeviceClock::time_point, DeviceClock::time_point>
    await_completed() const;
};

} // namespace low_latency

#endif
#ifndef SUBMISSIONS_HH_
#define SUBMISSIONS_HH_

#include "device_clock.hh"
#include "timestamp_pool.hh"

namespace low_latency {

struct Submission {
    std::shared_ptr<TimestampPool::Handle> start, end;
    DeviceClock::time_point_t time;
};

} // namespace low_latency

#endif